// =============================================================================
//  EngineImpl.hpp — Concrete engine. Internal header: never exposed to Swift or
//  JNI, and deliberately absent from the module map so backend types (ggml
//  contexts, llama_context, sqlite3*) can never leak across the bridge.
// =============================================================================
#pragma once

#include "NoteStore.hpp"
#include "SpeakerAttribution.hpp"
#include "SpeakerDiarizer.hpp"
#include "scribatic/core/AudioRingBuffer.hpp"
#include "scribatic/core/EngineInterface.hpp"
#include "scribatic/core/ModelResidency.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

struct whisper_context;

namespace scribatic::core {

class EngineImpl final : public EngineInterface {
public:
    explicit EngineImpl(EngineConfig config);
    ~EngineImpl() override;

    EngineStatus warmUp() noexcept override;
    void         hibernate() noexcept override;
    EngineState  state() const noexcept override;

    void         beginSession() noexcept override;
    std::int64_t saveSession(const std::string& title, std::int64_t createdAt,
                             const std::string& audioPath) override;

    EngineStatus pushAudio(const float* pcmFrames, std::size_t frameCount) noexcept override;
    EngineStatus runTranscriptionPass() noexcept override;
    EngineStatus flush() noexcept override;

    std::vector<TranscriptSegment> drainSegments() override;

    EngineStatus              indexNote(std::int64_t noteId, const std::string& text) override;
    std::vector<RetrievalHit> search(const std::string& query, std::int32_t topK) override;

    bool         canIdentifySpeakers() const noexcept override;
    EngineStatus identifySpeakers(std::int64_t noteId, std::int32_t expectedSpeakers) override;
    EngineStatus renameSpeaker(std::int64_t noteId, std::int32_t speaker,
                               const std::string& name) override;
    EngineStatus setNoteSummary(std::int64_t noteId, const std::string& summary) override;
    void         setLanguage(const std::string& code) override;
    bool         canRefine() const noexcept override;
    EngineStatus refineTranscript(std::int64_t noteId) override;
    float        refineProgress() const noexcept override;
    void         cancelRefine() noexcept override;
    EngineStatus setSegmentSpeaker(std::int64_t noteId, std::int64_t segmentId,
                                   std::int32_t speaker) override;
    EngineStatus mergeSpeakers(std::int64_t noteId, const SpeakerList& speakers,
                               std::int32_t into) override;
    EngineStatus setNoteLayout(std::int64_t noteId, const std::string& layout) override;
    std::string  sessionLanguage() override;

    std::vector<NoteSummary> listNotes() override;
    NoteDetail               loadNote(std::int64_t noteId) override;
    EngineStatus             deleteRecording(std::int64_t noteId) override;
    EngineStatus             deleteNote(std::int64_t noteId) override;
    std::string exportTranscript(std::int64_t noteId, bool includeTimestamps,
                                 bool anonymiseSpeakers) override;

    void requestCancel() noexcept override;

    // Reference count driven by Swift ARC / Kotlin handle ownership.
    void retain() noexcept { refCount_.fetch_add(1, std::memory_order_relaxed); }
    void release() noexcept;

private:
    /// 16 kHz mono; 30 s of headroom matches the whisper encoder window.
    static constexpr std::size_t kSampleRate     = 16000;
    static constexpr std::size_t kRingCapacity   = kSampleRate * 30;
    static constexpr std::size_t kEmbeddingDims  = 384;

    /// The UI polls a pass roughly every 400 ms. Handing whisper 400 ms of
    /// audio at a time would be both wasteful and inaccurate — the model has
    /// almost no context to work with and re-runs its encoder for a fraction of
    /// a word. Audio is accumulated until there is a window worth decoding.
    static constexpr std::size_t kWindowSamples  = kSampleRate * 5;

    /// A final flush still needs enough audio to be worth a pass; below this it
    /// is noise or a stray syllable.
    static constexpr std::size_t kMinFlushSamples = kSampleRate / 2;

    /// RMS below which a window is treated as silence and never decoded:
    /// -60 dBFS, far under any speech a phone microphone picks up. Whisper
    /// does not return nothing for silence — it tends to invent a short
    /// phrase ("you", "Thank you."), and a quiet room would fill the
    /// transcript with them.
    static constexpr float kSilenceRms = 0.001F;

    /// Runs whisper over `window_`, appending finalised segments. Caller must
    /// not hold segmentMutex_.
    EngineStatus decodeWindow(bool flushing) noexcept;

    /// Reconciles the store with the recordings directory: forgets recordings
    /// whose file is gone, and deletes files no note refers to — the leftovers
    /// of a crash between a file operation and the database write after it.
    void sweepRecordings();

    /// After speakers are known: a speaker whose own speech is confidently in
    /// another language than the recording's — a bilingual meeting, say — has
    /// their lines transcribed again, from the audio, in that language. The
    /// live transcript had them in the recording's first language, which
    /// whisper translates at best and garbles at worst.
    void transcribeSpeakersInTheirLanguage(Attribution& attribution,
                                           const std::vector<SpeakerTurn>& turns,
                                           const float* samples, std::size_t count,
                                           const std::string& recordingLanguage);

    /// whisper over a stretch of the recording in a given language, as text.
    std::string transcribeSpan(whisper_context* ctx, const float* samples, std::size_t count,
                               const std::string& language);

    /// A whisper context of its own, configured as the live one is, for work
    /// that must not share the live model (it may be transcribing meanwhile).
    [[nodiscard]] whisper_context* loadWhisper(const std::string& path) const;

    std::mutex         refineMutex_;   // one refinement at a time: ~0.5 GB each
    std::atomic<bool>  refineCancel_{false};
    std::atomic<float> refineProgress_{0.0F};

    [[nodiscard]] std::string recordingPath(const std::string& name) const;

    EngineConfig    config_;
    AudioRingBuffer ring_{kRingCapacity};
    ModelResidency  whisperWeights_;

    std::atomic<EngineState> state_{EngineState::Idle};
    std::atomic<bool>        cancelRequested_{false};
    std::atomic<int>         refCount_{1};

    // Scratch PCM staging buffer: preallocated in the constructor so the
    // inference pass performs zero heap traffic per iteration.
    std::vector<float> scratch_;

    std::mutex                    segmentMutex_;
    std::deque<TranscriptSegment> pendingSegments_;

    /// Everything the current session has produced, drained or not, guarded
    /// by segmentMutex_. The segments are what the note is saved with; the
    /// words are what speakers are later attributed from.
    std::vector<TranscriptSegment> sessionSegments_;
    std::vector<WordTiming>        sessionWords_;
    std::int32_t                   sessionSegmentCount_ = 0;

    /// "auto" or a code; and the current session's, empty until detected.
    /// Set from the UI thread, read on the engine's.
    std::mutex  languageMutex_;
    std::string languagePreference_ = "auto";
    std::string sessionLanguage_;

    NoteStore       store_;
    SpeakerDiarizer diarizer_;

    /// Audio waiting for a decode, and how many samples have already been
    /// decoded before it. whisper reports timestamps relative to the window it
    /// was given, so the offset is what makes them absolute for the transcript.
    std::vector<float> window_;
    std::int64_t       decodedSamples_ = 0;

    /// Owned whisper handle. Declared as an opaque pointer so this header pulls
    /// in no backend type even though it is already private to the library.
    whisper_context* whisper_ = nullptr;

    // TODO(backend): llama_context* / sqlite3* handles land here next.
};

} // namespace scribatic::core
