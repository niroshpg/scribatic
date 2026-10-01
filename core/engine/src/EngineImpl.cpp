// =============================================================================
//  EngineImpl.cpp — Pipeline skeleton.
//
//  Backend calls are marked TODO(backend) and are wired up after
//  `make setup-all` vendors whisper.cpp / llama.cpp into core/engine/vendor.
//  Everything structural — threading contract, allocation discipline, state
//  machine, cancellation — is implemented here and is what the platform layers
//  are written against.
// =============================================================================
#include "EngineImpl.hpp"

#include "RecordingAudio.hpp"
#include "TranscriptFormat.hpp"
#include "whisper.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstdlib>
#include <unordered_set>
#include <utility>

namespace scribatic::core {
namespace {

bool fileExists(const std::string& path) noexcept {
    struct stat info {};
    return !path.empty() && ::stat(path.c_str(), &info) == 0;
}

std::string baseName(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

/// Only files the apps themselves name are ever swept. Anything else in the
/// directory is not ours to delete, however it got there.
bool isRecordingFileName(const std::string& name) {
    constexpr std::string_view kPrefix = "recording-";
    constexpr std::string_view kSuffix = ".wav";
    return name.size() > kPrefix.size() + kSuffix.size() &&
           name.compare(0, kPrefix.size(), kPrefix) == 0 &&
           name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0;
}

/// Alignment heads for DTW word timing. Each whisper size has its own set of
/// cross-attention heads that track alignment, and a preset for the wrong size
/// reads the wrong heads, so the preset is keyed off the model's file name.
/// Unrecognised names fall back to the top text layers, which works with any
/// size but places words less precisely.
whisper_alignment_heads_preset alignmentPreset(const std::string& modelPath) {
    const std::string name = baseName(modelPath);
    const auto has = [&name](const char* token) { return name.find(token) != std::string::npos; };
    if (has("large-v3-turbo")) { return WHISPER_AHEADS_LARGE_V3_TURBO; }
    if (has("large-v3"))       { return WHISPER_AHEADS_LARGE_V3; }
    if (has("large-v2"))       { return WHISPER_AHEADS_LARGE_V2; }
    if (has("large-v1"))       { return WHISPER_AHEADS_LARGE_V1; }
    if (has("medium.en"))      { return WHISPER_AHEADS_MEDIUM_EN; }
    if (has("medium"))         { return WHISPER_AHEADS_MEDIUM; }
    if (has("small.en"))       { return WHISPER_AHEADS_SMALL_EN; }
    if (has("small"))          { return WHISPER_AHEADS_SMALL; }
    if (has("base.en"))        { return WHISPER_AHEADS_BASE_EN; }
    if (has("base"))           { return WHISPER_AHEADS_BASE; }
    if (has("tiny.en"))        { return WHISPER_AHEADS_TINY_EN; }
    if (has("tiny"))           { return WHISPER_AHEADS_TINY; }
    return WHISPER_AHEADS_N_TOP_MOST;
}

/// Removes a file; a file that is already gone counts as removed.
bool removeFile(const std::string& path) noexcept {
    return ::unlink(path.c_str()) == 0 || errno == ENOENT;
}

} // namespace

// -- Factory & lifetime -------------------------------------------------------

EngineInterface* EngineInterface::create(const EngineConfig& config,
                                         EngineStatus* outStatus) noexcept {
    const auto fail = [outStatus](EngineStatus status) -> EngineInterface* {
        if (outStatus != nullptr) { *outStatus = status; }
        return nullptr;
    };

    // Only the acoustic model is mandatory. The instruct model is optional by
    // design (the user may decline its 1.2 GB), and nothing reads it yet.
    if (!fileExists(config.whisperModelPath)) {
        return fail(EngineStatus::ModelNotFound);
    }

    auto* engine = new (std::nothrow) EngineImpl(config);
    if (engine == nullptr) {
        return fail(EngineStatus::OutOfMemory);
    }
    if (outStatus != nullptr) { *outStatus = EngineStatus::Ok; }
    return engine;
}


EngineImpl::EngineImpl(EngineConfig config) : config_(std::move(config)) {
    // Reserve the inference staging buffer up front. Steady-state transcription
    // must not touch the allocator: repeated large allocate/free cycles are the
    // primary source of heap fragmentation on 4 GB Android devices.
    scratch_.reserve(kRingCapacity);
}

EngineImpl::~EngineImpl() { hibernate(); }

void EngineImpl::release() noexcept {
    if (refCount_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete this;
    }
}

// -- Lifecycle ----------------------------------------------------------------

EngineStatus EngineImpl::warmUp() noexcept {
    state_.store(EngineState::Warming, std::memory_order_release);

    // The store opens first: it is cheap, and a note list that works while the
    // weights are still mapping is better than one that waits on them.
    if (!config_.databasePath.empty() && !store_.isOpen()) {
        if (store_.open(config_.databasePath) != EngineStatus::Ok) {
            state_.store(EngineState::Faulted, std::memory_order_release);
            return EngineStatus::DatabaseFailed;
        }
        // Only at first open, never on a re-warm after hibernate: a sweep
        // while a recording is being written would delete that recording.
        sweepRecordings();
    }

    if (config_.useMemoryMapping) {
        if (!whisperWeights_.map(config_.whisperModelPath)) {
            state_.store(EngineState::Faulted, std::memory_order_release);
            return EngineStatus::ModelLoadFailed;
        }
    }

    if (whisper_ == nullptr) {
        if (!whisperWeights_.valid()) {
            state_.store(EngineState::Faulted, std::memory_order_release);
            return EngineStatus::ModelLoadFailed;
        }

        whisper_context_params cparams = whisper_context_default_params();
        // The weights are already resident as a clean, file-backed mapping;
        // letting whisper mmap the path again would double the footprint for
        // no benefit. See ADR-004.
        cparams.use_gpu = false;

        // DTW token timing: where in the audio each token was actually
        // spoken, which is what word-level speaker attribution depends on.
        // The plain timestamp heuristic drifts by hundreds of milliseconds at
        // exactly the moments that matter — a change of speaker.
        cparams.dtw_token_timestamps = true;
        // whisper silently turns DTW off when flash attention is on, and
        // flash attention defaults to on. The attention weights DTW reads are
        // exactly what the fused kernel never materialises.
        cparams.flash_attn           = false;
        cparams.dtw_aheads_preset    = alignmentPreset(config_.whisperModelPath);
        cparams.dtw_n_top            = 2;   // only read by the N_TOP_MOST fallback
        // Scratch for the alignment pass, allocated on every decode. The
        // default is 128 MB, sized for 30 s windows; ours are about 5 s, and
        // 8 MB was measured to be enough. 32 MB leaves room for a window that
        // grew because decoding fell behind.
        cparams.dtw_mem_size         = std::size_t{32} * 1024 * 1024;
        if (const char* forced = std::getenv("SCRIBATIC_DTW_TOP_MOST");
            forced != nullptr && *forced == '1') {
            cparams.dtw_aheads_preset = WHISPER_AHEADS_N_TOP_MOST;
        }

        // whisper only reads from the buffer, but the C API takes void*. The
        // mapping is PROT_READ, so a write here would fault rather than
        // silently corrupt — which is the failure mode we want if that ever
        // stops being true.
        whisper_ = whisper_init_from_buffer_with_params(
            const_cast<void*>(whisperWeights_.data()),
            whisperWeights_.size(),
            cparams);

        if (whisper_ == nullptr) {
            state_.store(EngineState::Faulted, std::memory_order_release);
            return EngineStatus::ModelLoadFailed;
        }
    }

    state_.store(EngineState::Listening, std::memory_order_release);
    return EngineStatus::Ok;
}

void EngineImpl::hibernate() noexcept {
    // Order matters: the whisper context holds pointers INTO the mapped
    // weights, so freeing it after munmap would be a use-after-free on a
    // region the kernel has already taken back.
    if (whisper_ != nullptr) {
        whisper_free(whisper_);
        whisper_ = nullptr;
    }

    // ONNX Runtime sessions hold their own copies of the diarization weights
    // on the heap, so they go too. They reload on the next identification.
    diarizer_.unload();

    whisperWeights_.unmap();

    window_.clear();
    window_.shrink_to_fit();
    decodedSamples_ = 0;

    ring_.reset();
    state_.store(EngineState::Idle, std::memory_order_release);
}

EngineState EngineImpl::state() const noexcept {
    return state_.load(std::memory_order_acquire);
}

// -- Sessions -----------------------------------------------------------------

void EngineImpl::beginSession() noexcept {
    {
        std::lock_guard<std::mutex> lock(languageMutex_);
        sessionLanguage_ = languagePreference_ == "auto" ? std::string() : languagePreference_;
    }
    ring_.reset();
    window_.clear();
    decodedSamples_ = 0;
    cancelRequested_.store(false, std::memory_order_release);

    std::lock_guard<std::mutex> lock(segmentMutex_);
    pendingSegments_.clear();
    sessionSegments_.clear();
    sessionWords_.clear();
    sessionSegmentCount_ = 0;
}

std::int64_t EngineImpl::saveSession(const std::string& title, std::int64_t createdAt,
                                     const std::string& audioPath) {
    std::vector<TranscriptSegment> segments;
    std::vector<WordTiming> words;
    {
        std::lock_guard<std::mutex> lock(segmentMutex_);
        segments = sessionSegments_;
        words = sessionWords_;
    }
    // Everything pushed, including a tail too short for the final decode.
    const auto samples = decodedSamples_ + static_cast<std::int64_t>(window_.size());
    const std::int64_t durationMs = samples * 1000 / static_cast<std::int64_t>(kSampleRate);

    // Never detected (no speech, or an English-only model): English, which
    // is also what such a model wrote.
    std::string language = sessionLanguage();
    if (language.empty() || (whisper_ != nullptr && whisper_is_multilingual(whisper_) == 0)) {
        language = "en";
    }
    return store_.insertNote(title, createdAt, durationMs,
                             audioPath.empty() ? std::string() : baseName(audioPath),
                             segments, words, language);
}

void EngineImpl::setLanguage(const std::string& code) {
    std::lock_guard<std::mutex> lock(languageMutex_);
    languagePreference_ = code.empty() ? std::string("auto") : code;
}

std::string EngineImpl::sessionLanguage() {
    std::lock_guard<std::mutex> lock(languageMutex_);
    return sessionLanguage_;
}

// -- Realtime path ------------------------------------------------------------

EngineStatus EngineImpl::pushAudio(const float* pcmFrames, std::size_t frameCount) noexcept {
    // Audio-callback thread. No locks, no allocation, no logging.
    if (pcmFrames == nullptr || frameCount == 0) {
        return EngineStatus::UnsupportedFormat;
    }
    const std::size_t written = ring_.write(pcmFrames, frameCount);
    return written == frameCount ? EngineStatus::Ok : EngineStatus::AudioBufferOverrun;
}

// -- Inference ----------------------------------------------------------------

EngineStatus EngineImpl::runTranscriptionPass() noexcept {
    if (!whisperWeights_.valid() && config_.useMemoryMapping) {
        return EngineStatus::NotInitialized;
    }

    const std::size_t ready = ring_.available();
    if (ready == 0) {
        return EngineStatus::Ok;
    }

    state_.store(EngineState::Transcribing, std::memory_order_release);
    scratch_.resize(ready);                 // capacity already reserved
    ring_.read(scratch_.data(), ready);

    // Accumulate rather than decode immediately: see kWindowSamples.
    window_.insert(window_.end(), scratch_.begin(), scratch_.end());

    EngineStatus status = EngineStatus::Ok;
    if (window_.size() >= kWindowSamples) {
        status = decodeWindow(/*flushing=*/false);
    }

    state_.store(EngineState::Listening, std::memory_order_release);
    if (cancelRequested_.load(std::memory_order_acquire)) {
        return EngineStatus::Cancelled;
    }
    return status;
}

EngineStatus EngineImpl::flush() noexcept {
    // Anything still in the ring belongs to this recording too.
    const std::size_t ready = ring_.available();
    if (ready > 0) {
        scratch_.resize(ready);
        ring_.read(scratch_.data(), ready);
        window_.insert(window_.end(), scratch_.begin(), scratch_.end());
    }

    const EngineStatus status = decodeWindow(/*flushing=*/true);
    state_.store(EngineState::Listening, std::memory_order_release);
    return status;
}

EngineStatus EngineImpl::decodeWindow(bool flushing) noexcept {
    if (whisper_ == nullptr) {
        return EngineStatus::NotInitialized;
    }

    const std::size_t minimum = flushing ? kMinFlushSamples : kWindowSamples;
    if (window_.size() < minimum) {
        return EngineStatus::Ok;
    }

    // Silence is skipped, not decoded (see kSilenceRms). Its time still
    // counts, so what is said after it keeps its place in the recording.
    double energy = 0.0;
    for (const float sample : window_) { energy += static_cast<double>(sample) * sample; }
    if (std::sqrt(energy / static_cast<double>(window_.size())) < kSilenceRms) {
        decodedSamples_ += static_cast<std::int64_t>(window_.size());
        window_.clear();
        return EngineStatus::Ok;
    }

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.n_threads        = config_.threadCount > 0 ? config_.threadCount : 4;
    params.translate        = false;
    // A multilingual model is told the session's language once it is known,
    // and detects it until then; an English-only model only has English.
    const bool multilingual = whisper_is_multilingual(whisper_) != 0;
    std::string language = multilingual ? sessionLanguage() : std::string("en");
    const bool detecting = language.empty();
    params.language         = detecting ? "auto" : language.c_str();
    params.detect_language  = false;
    params.no_timestamps    = false;
    // Nothing is allowed to write to stdout from inside a mobile app.
    params.print_progress   = false;
    params.print_realtime   = false;
    params.print_timestamps = false;
    params.print_special    = false;
    // Per-token times are what speaker attribution cuts segments on: whisper
    // segments on pauses, not voices, and a segment often spans two people.
    params.token_timestamps = true;
    // Observed between graph nodes, so a cancel does not have to wait out a
    // whole decode. Returning true aborts.
    params.abort_callback = [](void* user) -> bool {
        auto* self = static_cast<EngineImpl*>(user);
        return self->cancelRequested_.load(std::memory_order_acquire);
    };
    params.abort_callback_user_data = this;

    const int rc = whisper_full(whisper_, params,
                                window_.data(),
                                static_cast<int>(window_.size()));
    if (rc != 0) {
        return cancelRequested_.load(std::memory_order_acquire) ? EngineStatus::Cancelled
                                                                : EngineStatus::InferenceFailed;
    }

    // whisper timestamps are centiseconds relative to the window it was handed,
    // so the offset is what places them in the recording as a whole.
    const std::int64_t offsetMs = (decodedSamples_ * 1000) / static_cast<std::int64_t>(kSampleRate);

    const int count = whisper_full_n_segments(whisper_);
    const whisper_token endOfText = whisper_token_eot(whisper_);
    std::vector<TranscriptSegment> decoded;
    std::vector<std::vector<WordTiming>> decodedWords;
    decoded.reserve(static_cast<std::size_t>(count));
    decodedWords.reserve(static_cast<std::size_t>(count));

    for (int i = 0; i < count; ++i) {
        const char* text = whisper_full_get_segment_text(whisper_, i);
        if (text == nullptr) { continue; }

        std::string trimmed(text);
        const auto first = trimmed.find_first_not_of(" \t\n");
        if (first == std::string::npos) { continue; }   // silence decodes to blanks
        trimmed.erase(0, first);
        const auto last = trimmed.find_last_not_of(" \t\n");
        if (last != std::string::npos) { trimmed.erase(last + 1); }

        // whisper annotates non-speech rather than emitting nothing:
        // "[BLANK_AUDIO]", "[SOUND]", "(upbeat music)". Those are the model
        // describing the audio, not a transcript of it, and a pause in dictation
        // should leave the transcript untouched rather than push a marker into
        // it. The bracket convention is how they are distinguished.
        const bool isAnnotation =
            (trimmed.front() == '[' && trimmed.back() == ']') ||
            (trimmed.front() == '(' && trimmed.back() == ')');
        if (isAnnotation) { continue; }

        TranscriptSegment segment;
        segment.startMs = offsetMs + whisper_full_get_segment_t0(whisper_, i) * 10;
        segment.endMs   = offsetMs + whisper_full_get_segment_t1(whisper_, i) * 10;
        segment.text    = std::move(trimmed);
        segment.isFinal = true;

        // Mean token probability, which is what the UI dims a provisional
        // segment on. Averaged rather than multiplied: a long segment would
        // otherwise round to zero regardless of how confident it was.
        const int tokens = whisper_full_n_tokens(whisper_, i);
        float sum = 0.0F;
        for (int t = 0; t < tokens; ++t) {
            sum += whisper_full_get_token_p(whisper_, i, t);
        }
        segment.confidence = tokens > 0 ? sum / static_cast<float>(tokens) : 0.0F;

        // Words, from BPE pieces: a piece with a leading space starts a new
        // word, anything else continues the current one. Special tokens
        // (timestamps, start/end markers) sit at or above end-of-text.
        std::vector<WordTiming> words;
        std::vector<int> piecesPerWord;
        for (int t = 0; t < tokens; ++t) {
            const whisper_token_data data = whisper_full_get_token_data(whisper_, i, t);
            if (data.id >= endOfText) { continue; }
            const char* piece = whisper_full_get_token_text(whisper_, i, t);
            if (piece == nullptr || *piece == '\0') { continue; }

            // DTW gives the moment each token was spoken; the heuristic t0/t1
            // pair is the fallback when alignment did not run. Either way,
            // keep times inside the segment and never running backwards.
            const std::int64_t spoken = data.t_dtw >= 0 ? data.t_dtw : data.t0;
            const std::int64_t t0 = std::clamp<std::int64_t>(offsetMs + spoken * 10,
                                                             segment.startMs, segment.endMs);
            const std::int64_t t1 = std::clamp<std::int64_t>(offsetMs + data.t1 * 10,
                                                             t0, segment.endMs);

            if (piece[0] == ' ' || words.empty()) {
                WordTiming word;
                word.startMs = words.empty() ? t0 : std::max(t0, words.back().endMs);
                word.endMs = std::max(t1, word.startMs);
                word.text = piece;
                word.probability = data.p;
                words.push_back(std::move(word));
                piecesPerWord.push_back(1);
            } else {
                words.back().text += piece;
                words.back().endMs = std::max(words.back().endMs, t1);
                words.back().probability += data.p;
                ++piecesPerWord.back();
            }
        }
        for (std::size_t w = 0; w < words.size(); ++w) {
            words[w].probability /= static_cast<float>(piecesPerWord[w]);
            // A word lasts until the next one starts. DTW marks onsets only,
            // and an onset-to-onset span is what overlap with a turn needs.
            words[w].endMs = w + 1 < words.size() ? std::max(words[w + 1].startMs, words[w].startMs)
                                                  : segment.endMs;
        }

        decoded.push_back(std::move(segment));
        decodedWords.push_back(std::move(words));
    }

    decodedSamples_ += static_cast<std::int64_t>(window_.size());
    window_.clear();

    // The first window with speech decides the recording's language. One
    // with nothing in it says nothing about the language: keep detecting.
    if (detecting && !decoded.empty()) {
        const int id = whisper_full_lang_id(whisper_);
        const char* code = id >= 0 ? whisper_lang_str(id) : nullptr;
        if (code != nullptr) {
            std::lock_guard<std::mutex> lock(languageMutex_);
            if (sessionLanguage_.empty()) { sessionLanguage_ = code; }
        }
    }

    if (!decoded.empty()) {
        std::lock_guard<std::mutex> lock(segmentMutex_);
        for (std::size_t k = 0; k < decoded.size(); ++k) {
            const std::int32_t ordinal = sessionSegmentCount_++;
            for (auto& word : decodedWords[k]) {
                word.segment = ordinal;
                sessionWords_.push_back(std::move(word));
            }
            sessionSegments_.push_back(decoded[k]);
            pendingSegments_.push_back(std::move(decoded[k]));
        }
    }

    return EngineStatus::Ok;
}

std::vector<TranscriptSegment> EngineImpl::drainSegments() {
    std::lock_guard<std::mutex> lock(segmentMutex_);
    std::vector<TranscriptSegment> out(pendingSegments_.begin(), pendingSegments_.end());
    pendingSegments_.clear();
    return out;
}

EngineStatus EngineImpl::setNoteSummary(std::int64_t noteId, const std::string& summary) {
    return store_.setSummary(noteId, summary);
}

// -- Retrieval ----------------------------------------------------------------

EngineStatus EngineImpl::indexNote(std::int64_t noteId, const std::string& text) {
    // TODO(backend): chunk -> embed (kEmbeddingDims) -> INSERT into vss_chunks.
    (void)noteId;
    (void)text;
    return EngineStatus::Ok;
}

std::vector<RetrievalHit> EngineImpl::search(const std::string& query, std::int32_t topK) {
    // TODO(backend): embed query, then `SELECT ... FROM vss_chunks
    // WHERE vss_search(embedding, ?) LIMIT ?`.
    (void)query;
    (void)topK;
    return {};
}

// -- Speakers -----------------------------------------------------------------

bool EngineImpl::canIdentifySpeakers() const noexcept {
    return SpeakerDiarizer::compiledIn() && fileExists(config_.segmentationModelPath) &&
           fileExists(config_.speakerEmbeddingModelPath);
}

EngineStatus EngineImpl::identifySpeakers(std::int64_t noteId, std::int32_t expectedSpeakers) {
    if (!canIdentifySpeakers()) { return EngineStatus::SpeakersUnavailable; }

    const std::string name = store_.audioName(noteId);
    RecordingAudio audio;
    if (name.empty() || !audio.open(recordingPath(name))) {
        return EngineStatus::RecordingNotFound;
    }

    EngineStatus status = EngineStatus::Ok;
    const auto turns = diarizer_.run(config_.segmentationModelPath,
                                     config_.speakerEmbeddingModelPath, config_.threadCount,
                                     audio.samples(), audio.count(), expectedSpeakers, &status);
    if (status != EngineStatus::Ok) { return status; }

    const auto words = store_.words(noteId);
    if (words.empty()) { return EngineStatus::Ok; }   // nothing was said to attribute

    const Attribution attribution = attributeSpeakers(words, turns);
    return store_.replaceTranscript(noteId, attribution.segments, attribution.speakerCount);
}

EngineStatus EngineImpl::renameSpeaker(std::int64_t noteId, std::int32_t speaker,
                                       const std::string& name) {
    return store_.renameSpeaker(noteId, speaker, name);
}

// -- Notes --------------------------------------------------------------------

std::vector<NoteSummary> EngineImpl::listNotes() { return store_.list(); }

NoteDetail EngineImpl::loadNote(std::int64_t noteId) {
    return store_.load(noteId, config_.recordingsDirectory);
}

EngineStatus EngineImpl::deleteRecording(std::int64_t noteId) {
    const std::string name = store_.audioName(noteId);
    if (name.empty()) { return EngineStatus::Ok; }
    if (!removeFile(recordingPath(name))) { return EngineStatus::FileRemovalFailed; }
    return store_.clearAudio(noteId);
}

EngineStatus EngineImpl::deleteNote(std::int64_t noteId) {
    std::string name;
    const EngineStatus status = store_.deleteNote(noteId, &name);
    if (status != EngineStatus::Ok) { return status; }
    // The row is gone, so a file that will not delete is now unreferenced —
    // and the next sweep removes unreferenced recordings. Still reported, so
    // the UI does not claim the audio is gone while it is on disk.
    if (!name.empty() && !removeFile(recordingPath(name))) {
        return EngineStatus::FileRemovalFailed;
    }
    return EngineStatus::Ok;
}

std::string EngineImpl::exportTranscript(std::int64_t noteId, bool includeTimestamps,
                                         bool anonymiseSpeakers) {
    const NoteDetail note = loadNote(noteId);
    if (note.id == 0) { return {}; }
    return formatTranscript(note, ExportOptions{includeTimestamps, anonymiseSpeakers});
}

std::string EngineImpl::recordingPath(const std::string& name) const {
    return config_.recordingsDirectory + "/" + name;
}

void EngineImpl::sweepRecordings() {
    if (config_.recordingsDirectory.empty()) { return; }

    std::unordered_set<std::string> referenced;
    for (const auto& reference : store_.audioReferences()) {
        if (fileExists(recordingPath(reference.audioName))) {
            referenced.insert(reference.audioName);
        } else {
            store_.clearAudio(reference.noteId);
        }
    }

    DIR* directory = ::opendir(config_.recordingsDirectory.c_str());
    if (directory == nullptr) { return; }
    while (const dirent* entry = ::readdir(directory)) {
        const std::string name = entry->d_name;
        if (isRecordingFileName(name) && referenced.count(name) == 0) {
            (void)removeFile(recordingPath(name));
        }
    }
    ::closedir(directory);
}

void EngineImpl::requestCancel() noexcept {
    cancelRequested_.store(true, std::memory_order_release);
}

// -- Diagnostics --------------------------------------------------------------

std::string describeStatus(EngineStatus status) {
    switch (status) {
        case EngineStatus::Ok:                 return "ok";
        case EngineStatus::NotInitialized:     return "engine not initialized";
        case EngineStatus::ModelNotFound:      return "model file not found";
        case EngineStatus::ModelLoadFailed:    return "model load failed";
        case EngineStatus::UnsupportedFormat:  return "unsupported audio format";
        case EngineStatus::AudioBufferOverrun: return "audio ring buffer overrun";
        case EngineStatus::InferenceFailed:    return "inference failed";
        case EngineStatus::DatabaseFailed:     return "database failure";
        case EngineStatus::Cancelled:          return "cancelled";
        case EngineStatus::OutOfMemory:        return "out of memory";
        case EngineStatus::RecordingNotFound:  return "the recording is no longer on this device";
        case EngineStatus::SpeakersUnavailable:return "speaker identification is not available";
        case EngineStatus::FileRemovalFailed:  return "the recording could not be deleted";
    }
    return "unknown";
}

} // namespace scribatic::core

// Global scope, matching the declarations in EngineInterface.hpp — Swift's
// SWIFT_SHARED_REFERENCE looks these two names up in the global namespace.
void scribaticEngineRetain(scribatic::core::EngineInterface* engine) noexcept {
    if (auto* impl = static_cast<scribatic::core::EngineImpl*>(engine)) { impl->retain(); }
}

void scribaticEngineRelease(scribatic::core::EngineInterface* engine) noexcept {
    if (auto* impl = static_cast<scribatic::core::EngineImpl*>(engine)) { impl->release(); }
}
