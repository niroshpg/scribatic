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
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cctype>
#include <cmath>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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

/// A recording the app never stopped still has the RIFF and `data` sizes the
/// writer only fills in on close. Sets them from what reached the disk, less a
/// trailing partial sample, so a player reads it as an ordinary WAV. A file
/// whose sizes were already written is left as it is.
bool finishWavHeader(const std::string& path) noexcept {
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) { return false; }
    struct Close { int fd; ~Close() { ::close(fd); } } closeAtEnd{fd};

    struct stat info {};
    if (::fstat(fd, &info) != 0) { return false; }
    const auto size = static_cast<std::uint64_t>(info.st_size);
    unsigned char riff[12];
    if (::pread(fd, riff, sizeof riff, 0) != static_cast<ssize_t>(sizeof riff) ||
        std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) {
        return false;
    }

    std::uint16_t blockAlign = 4;   // mono float32, what both apps write
    std::uint64_t pos = 12;
    while (pos + 8 <= size) {
        unsigned char chunk[8];
        if (::pread(fd, chunk, sizeof chunk, static_cast<off_t>(pos)) != static_cast<ssize_t>(sizeof chunk)) {
            return false;
        }
        std::uint32_t chunkSize = 0;
        std::memcpy(&chunkSize, chunk + 4, 4);
        const std::uint64_t body = pos + 8;

        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            unsigned char align[2];
            if (::pread(fd, align, 2, static_cast<off_t>(body + 12)) == 2) {
                std::memcpy(&blockAlign, align, 2);
                if (blockAlign == 0) { blockAlign = 4; }
            }
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            if (chunkSize != 0 && chunkSize <= size - body) { return true; }
            const std::uint64_t dataBytes = (size - body) / blockAlign * blockAlign;
            if (body + dataBytes - 8 > UINT32_MAX) { return false; }
            const auto dataSize = static_cast<std::uint32_t>(dataBytes);
            const auto riffSize = static_cast<std::uint32_t>(body + dataBytes - 8);
            return ::ftruncate(fd, static_cast<off_t>(body + dataBytes)) == 0 &&
                   ::pwrite(fd, &dataSize, 4, static_cast<off_t>(pos + 4)) == 4 &&
                   ::pwrite(fd, &riffSize, 4, 4) == 4;
        }
        pos = body + chunkSize + (chunkSize & 1);
    }
    return false;
}

/// When a recording started, in Unix seconds: the millisecond stamp both apps
/// put in its name, or failing that the file's own time.
std::int64_t recordingStartedAt(const std::string& path, const std::string& name) {
    constexpr std::size_t kPrefix = sizeof("recording-") - 1;
    constexpr std::size_t kSuffix = sizeof(".wav") - 1;
    const std::string stamp = name.substr(kPrefix, name.size() - kPrefix - kSuffix);
    char* end = nullptr;
    const long long millis = std::strtoll(stamp.c_str(), &end, 10);
    if (!stamp.empty() && end != nullptr && *end == '\0' && millis > 0) { return millis / 1000; }
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0 ? static_cast<std::int64_t>(info.st_mtime) : 0;
}

/// Turns what whisper decoded into segments and their words, `offsetMs` being
/// where the decoded audio starts in the recording. Shared by the live
/// windows and the accurate pass after Stop.
/// `text` with anything that is not well-formed UTF-8 removed, U+FFFD
/// included. whisper's tokens are byte pieces, and a segment can end on half
/// of a character; shown, that is a replacement mark in the transcript, and fed to a
/// summary it reads as noise the model then explains.
std::string wellFormedUtf8(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
        bool ok = length > 0 && i + length <= text.size();
        for (std::size_t k = 1; ok && k < length; ++k) {
            ok = (static_cast<unsigned char>(text[i + k]) & 0xC0) == 0x80;
        }
        // U+FFFD (EF BF BD) is well formed, but it only ever marks a lost character.
        if (ok && length == 3 && text.compare(i, 3, "\xEF\xBF\xBD") == 0) { ok = false; }
        if (ok) { out.append(text, i, length); i += length; } else { ++i; }
    }
    return out;
}

void collectSegments(whisper_context* ctx, std::int64_t offsetMs,
                     std::vector<TranscriptSegment>& decoded,
                     std::vector<std::vector<WordTiming>>& decodedWords) {
    const int count = whisper_full_n_segments(ctx);
    const whisper_token endOfText = whisper_token_eot(ctx);
    decoded.reserve(static_cast<std::size_t>(count));
    decodedWords.reserve(static_cast<std::size_t>(count));

    for (int i = 0; i < count; ++i) {
        const char* text = whisper_full_get_segment_text(ctx, i);
        if (text == nullptr) { continue; }

        std::string trimmed = wellFormedUtf8(text);
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
        // Punctuation alone (".", "...") is whisper filling a pause, not
        // speech. Any byte above ASCII counts as a letter: non-Latin scripts.
        const bool saysSomething = std::any_of(trimmed.begin(), trimmed.end(), [](char c) {
            const auto byte = static_cast<unsigned char>(c);
            return byte >= 0x80 || std::isalnum(byte) != 0;
        });
        if (!saysSomething) { continue; }

        TranscriptSegment segment;
        segment.startMs = offsetMs + whisper_full_get_segment_t0(ctx, i) * 10;
        segment.endMs   = offsetMs + whisper_full_get_segment_t1(ctx, i) * 10;
        segment.text    = std::move(trimmed);
        segment.isFinal = true;

        // Mean token probability, which is what the UI dims a provisional
        // segment on. Averaged rather than multiplied: a long segment would
        // otherwise round to zero regardless of how confident it was.
        const int tokens = whisper_full_n_tokens(ctx, i);
        float sum = 0.0F;
        for (int t = 0; t < tokens; ++t) {
            sum += whisper_full_get_token_p(ctx, i, t);
        }
        segment.confidence = tokens > 0 ? sum / static_cast<float>(tokens) : 0.0F;

        // Words, from BPE pieces: a piece with a leading space starts a new
        // word, anything else continues the current one. Special tokens
        // (timestamps, start/end markers) sit at or above end-of-text.
        std::vector<WordTiming> words;
        std::vector<int> piecesPerWord;
        for (int t = 0; t < tokens; ++t) {
            const whisper_token_data data = whisper_full_get_token_data(ctx, i, t);
            if (data.id >= endOfText) { continue; }
            const char* piece = whisper_full_get_token_text(ctx, i, t);
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
        // while a recording is being written would take it for a recording
        // cut short, and recover it as a note of its own.
        sweepRecordings();
    }

    // Mapping again would unmap the weights from under a live context, so a
    // warm engine keeps them; after hibernate, whisper_ is null and they map.
    if (config_.useMemoryMapping && whisper_ == nullptr) {
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

    std::vector<TranscriptSegment> decoded;
    std::vector<std::vector<WordTiming>> decodedWords;
    collectSegments(whisper_, offsetMs, decoded, decodedWords);

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

// -- Search -------------------------------------------------------------------

std::vector<RetrievalHit> EngineImpl::search(const std::string& query, std::int32_t topK) {
    return store_.searchWords(query, topK);
}

std::vector<Passage> EngineImpl::passages(bool withoutEmbeddingOnly) {
    return store_.passages(withoutEmbeddingOnly);
}

EngineStatus EngineImpl::setPassageEmbedding(std::int64_t passageId, const Embedding& embedding) {
    return store_.setPassageEmbedding(passageId, embedding);
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

    Attribution attribution = attributeSpeakers(words, turns);
    if (attribution.speakerCount > 1) {
        transcribeSpeakersInTheirLanguage(attribution, turns, audio.samples(), audio.count(),
                                          loadNote(noteId).language);
    }
    return store_.replaceTranscript(noteId, attribution.segments, attribution.speakerCount);
}

void EngineImpl::transcribeSpeakersInTheirLanguage(Attribution& attribution,
                                                   const std::vector<SpeakerTurn>& turns,
                                                   const float* samples, std::size_t count,
                                                   const std::string& recordingLanguage) {
    if (count == 0) { return; }
    // A pinned language means the user said what everyone speaks.
    {
        std::lock_guard<std::mutex> lock(languageMutex_);
        if (languagePreference_ != "auto") { return; }
    }
    // A context of its own, never the live one: a new recording may be
    // transcribing while this note is finished in the background. The
    // accurate model when there is one: it detects and transcribes better.
    whisper_context* ctx = loadWhisper(canRefine() ? config_.accurateModelPath : config_.whisperModelPath);
    if (ctx == nullptr) { return; }
    struct Free { whisper_context* c; ~Free() { whisper_free(c); } } freeAtEnd{ctx};
    if (whisper_is_multilingual(ctx) == 0) { return; }
    const int threads = config_.threadCount > 0 ? config_.threadCount : 4;
    const auto sampleAt = [&](std::int64_t ms) {
        return std::min(count, static_cast<std::size_t>(std::max<std::int64_t>(0, ms)) * kSampleRate / 1000);
    };

    // Each cluster's turns, in order, with gaps under a second closed: the
    // diarizer cuts a turn at every breath, and whisper does badly on slivers.
    std::unordered_map<std::int32_t, std::vector<SpeakerTurn>> byCluster;
    for (const auto& turn : turns) {
        auto& list = byCluster[turn.speaker];
        if (!list.empty() && turn.startMs - list.back().endMs < 1000) {
            list.back().endMs = std::max(list.back().endMs, turn.endMs);
        } else {
            list.push_back(turn);
        }
    }

    std::vector<TranscriptSegment> replaced;
    std::vector<std::pair<std::int64_t, std::int64_t>> covered;
    for (auto& [cluster, list] : byCluster) {
        // The words were attributed from the live transcript, whose timing
        // for a translated stretch is off, so this works from the diarizer's
        // turns — the audio — instead. Up to 20 s of the speaker decides.
        std::vector<float> sample;
        for (const auto& turn : list) {
            sample.insert(sample.end(), samples + sampleAt(turn.startMs), samples + sampleAt(turn.endMs));
            if (sample.size() >= kSampleRate * 20) { break; }
        }
        // Under 3 s says too little about a language to overrule the recording's.
        if (sample.size() < kSampleRate * 3) { continue; }
        if (whisper_pcm_to_mel(ctx, sample.data(), static_cast<int>(sample.size()), threads) != 0) { continue; }
        std::vector<float> probs(static_cast<std::size_t>(whisper_lang_max_id() + 1), 0.0F);
        const int id = whisper_lang_auto_detect(ctx, 0, threads, probs.data());
        if (id < 0) { continue; }
        const std::string language = whisper_lang_str(id);
        // Confident, and not the recording's language: worth a second pass.
        if (language == recordingLanguage || probs[static_cast<std::size_t>(id)] < 0.6F) { continue; }

        std::int32_t speaker = 0;
        if (const auto known = attribution.speakerOfCluster.find(cluster); known != attribution.speakerOfCluster.end()) {
            speaker = known->second;
        } else {   // every word of theirs had gone to someone else
            speaker = attribution.speakerCount++;
            attribution.speakerOfCluster[cluster] = speaker;
        }
        for (const auto& turn : list) {
            const std::size_t from = sampleAt(turn.startMs), to = sampleAt(turn.endMs);
            if (to - from < kSampleRate / 2) { continue; }
            std::string text = transcribeSpan(ctx, samples + from, to - from, language);
            if (text.empty()) { continue; }
            TranscriptSegment segment;
            segment.startMs    = turn.startMs;
            segment.endMs      = turn.endMs;
            segment.text       = std::move(text);
            segment.speaker    = speaker;
            segment.isFinal    = true;
            segment.confidence = 1.0F;
            replaced.push_back(std::move(segment));
            covered.emplace_back(turn.startMs, turn.endMs);
        }
    }
    if (replaced.empty()) { return; }

    // Lines of the live transcript inside those turns are what was replaced,
    // whoever they had been attributed to.
    auto& segments = attribution.segments;
    segments.erase(std::remove_if(segments.begin(), segments.end(), [&](const TranscriptSegment& s) {
        const std::int64_t middle = (s.startMs + s.endMs) / 2;
        return std::any_of(covered.begin(), covered.end(),
                           [&](const auto& range) { return middle >= range.first && middle <= range.second; });
    }), segments.end());
    segments.insert(segments.end(), replaced.begin(), replaced.end());
    std::stable_sort(segments.begin(), segments.end(),
                     [](const TranscriptSegment& a, const TranscriptSegment& b) { return a.startMs < b.startMs; });
}

whisper_context* EngineImpl::loadWhisper(const std::string& path) const {
    if (!fileExists(path)) { return nullptr; }
    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu              = false;
    cparams.dtw_token_timestamps = true;   // word timings, for speakers
    cparams.flash_attn           = false;  // DTW needs the attention weights
    cparams.dtw_aheads_preset    = alignmentPreset(path);
    cparams.dtw_n_top            = 2;
    cparams.dtw_mem_size         = std::size_t{64} * 1024 * 1024;   // 30 s windows, not 5 s
    return whisper_init_from_file_with_params(path.c_str(), cparams);
}

bool EngineImpl::canRefine() const noexcept {
    try {
        return fileExists(config_.accurateModelPath);
    } catch (...) {
        return false;
    }
}

float EngineImpl::refineProgress() const noexcept {
    return refineProgress_.load(std::memory_order_relaxed);
}

void EngineImpl::cancelRefine() noexcept {
    refineCancel_.store(true, std::memory_order_release);
}

EngineStatus EngineImpl::refineTranscript(std::int64_t noteId) {
    if (!canRefine()) { return EngineStatus::ModelNotFound; }
    std::lock_guard<std::mutex> lock(refineMutex_);
    const NoteDetail note = loadNote(noteId);
    if (note.id == 0) { return EngineStatus::DatabaseFailed; }
    const std::string name = store_.audioName(noteId);
    RecordingAudio audio;
    if (name.empty() || !audio.open(recordingPath(name))) { return EngineStatus::RecordingNotFound; }

    refineCancel_.store(false, std::memory_order_release);
    refineProgress_.store(0.0F, std::memory_order_relaxed);
    whisper_context* ctx = loadWhisper(config_.accurateModelPath);
    if (ctx == nullptr) { return EngineStatus::ModelLoadFailed; }
    struct Free { whisper_context* c; ~Free() { whisper_free(c); } } freeAtEnd{ctx};

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.n_threads        = config_.threadCount > 0 ? config_.threadCount : 4;
    params.translate        = false;
    // Detecting automatically, this model decides the language again from
    // its first 30 s: the preview settled it from one 5 s window with the
    // live model, and quiet or brief speech there can mislead it. A language
    // the user chose is kept. Per-speaker languages are put right afterwards,
    // when speakers are identified.
    bool redetect = false;
    {
        std::lock_guard<std::mutex> languageLock(languageMutex_);
        redetect = languagePreference_ == "auto";
    }
    std::string language = "en";
    if (whisper_is_multilingual(ctx) != 0) {
        language = redetect || note.language.empty() || note.language == "auto" ? "auto" : note.language;
    }
    // Deciding: from the stretches that are speech, not the whole first 30 s.
    // Mostly silence detects as anything (a 9 s note with one short phrase in
    // it came back as Norwegian, and the phrase as noise in four scripts).
    // Under 3 s of speech, or unsure, the live pass's language stands.
    const bool detecting = language == "auto";
    if (detecting) {
        const bool known = !note.language.empty() && note.language != "auto";
        const int threads = params.n_threads;
        std::vector<float> speech;
        constexpr std::size_t kFrame = kSampleRate / 10;
        for (std::size_t at = 0; at + kFrame <= audio.count() && speech.size() < kSampleRate * 30; at += kFrame) {
            double energy = 0.0;
            for (std::size_t i = at; i < at + kFrame; ++i) { energy += static_cast<double>(audio.samples()[i]) * audio.samples()[i]; }
            if (std::sqrt(energy / kFrame) >= kSpeechRms) {
                speech.insert(speech.end(), audio.samples() + at, audio.samples() + at + kFrame);
            }
        }
        std::string chosen;
        if (speech.size() >= kSampleRate * 3 &&
            whisper_pcm_to_mel(ctx, speech.data(), static_cast<int>(speech.size()), threads) == 0) {
            std::vector<float> probs(static_cast<std::size_t>(whisper_lang_max_id() + 1), 0.0F);
            const int id = whisper_lang_auto_detect(ctx, 0, threads, probs.data());
            if (id >= 0 && (probs[static_cast<std::size_t>(id)] >= 0.6F || !known)) { chosen = whisper_lang_str(id); }
        }
        if (chosen.empty() && known) { chosen = note.language; }
        if (!chosen.empty()) { language = chosen; }
    }
    params.language         = language.c_str();
    params.no_timestamps    = false;
    params.token_timestamps = true;
    // Context across whisper's 30 s windows: what the live 5 s slices lack,
    // and most of why this pass is better (a sentence is no longer cut in two).
    params.no_context       = false;
    params.print_progress   = false;
    params.print_realtime   = false;
    params.print_timestamps = false;
    params.print_special    = false;
    params.progress_callback = [](whisper_context*, whisper_state*, int progress, void* user) {
        static_cast<std::atomic<float>*>(user)->store(static_cast<float>(progress) / 100.0F, std::memory_order_relaxed);
    };
    params.progress_callback_user_data = &refineProgress_;
    params.abort_callback = [](void* user) -> bool {
        return static_cast<std::atomic<bool>*>(user)->load(std::memory_order_acquire);
    };
    params.abort_callback_user_data = &refineCancel_;

    if (whisper_full(ctx, params, audio.samples(), static_cast<int>(audio.count())) != 0) {
        return refineCancel_.load(std::memory_order_acquire) ? EngineStatus::Cancelled : EngineStatus::InferenceFailed;
    }
    // The language this pass detected becomes the note's: a recovered
    // recording had none, and a live guess may have been wrong.
    std::string settled;
    if (language == "auto") {
        const int detected = whisper_full_lang_id(ctx);
        settled = detected >= 0 ? whisper_lang_str(detected) : "en";
    } else if (detecting || note.language == "auto") {
        settled = language;
    }
    std::vector<TranscriptSegment> decoded;
    std::vector<std::vector<WordTiming>> decodedWords;
    collectSegments(ctx, 0, decoded, decodedWords);

    // whisper invents a phrase for a window of silence ("you"), as the live
    // path found; anything said over near-silence is dropped here as well.
    std::vector<TranscriptSegment> segments;
    std::vector<WordTiming> words;
    for (std::size_t k = 0; k < decoded.size(); ++k) {
        const auto from = std::min(audio.count(), static_cast<std::size_t>(std::max<std::int64_t>(0, decoded[k].startMs)) * kSampleRate / 1000);
        const auto to = std::min(audio.count(), static_cast<std::size_t>(std::max<std::int64_t>(0, decoded[k].endMs)) * kSampleRate / 1000);
        double energy = 0.0;
        for (std::size_t i = from; i < to; ++i) { energy += static_cast<double>(audio.samples()[i]) * audio.samples()[i]; }
        if (to <= from || std::sqrt(energy / static_cast<double>(to - from)) < kSilenceRms) { continue; }
        const auto ordinal = static_cast<std::int32_t>(segments.size());
        for (auto& word : decodedWords[k]) { word.segment = ordinal; words.push_back(std::move(word)); }
        segments.push_back(std::move(decoded[k]));
    }
    refineProgress_.store(1.0F, std::memory_order_relaxed);
    return store_.replaceWithRefined(noteId, segments, words, settled);
}

EngineStatus EngineImpl::setSegmentSpeaker(std::int64_t noteId, std::int64_t segmentId, std::int32_t speaker) {
    return store_.setSegmentSpeaker(noteId, segmentId, speaker, nullptr);
}

EngineStatus EngineImpl::mergeSpeakers(std::int64_t noteId, const SpeakerList& speakers,
                                       std::int32_t into) {
    return store_.mergeSpeakers(noteId, speakers, into);
}

EngineStatus EngineImpl::editText(std::int64_t noteId, const SegmentIdList& segmentIds,
                                  const std::string& text) {
    return store_.editText(noteId, segmentIds, text);
}

EngineStatus EngineImpl::restoreText(std::int64_t noteId, const SegmentIdList& segmentIds) {
    return store_.restoreText(noteId, segmentIds);
}

EngineStatus EngineImpl::setNoteLayout(std::int64_t noteId, const std::string& layout) {
    if (layout != "auto" && layout != "discussion" && layout != "lecture") { return EngineStatus::InferenceFailed; }
    return store_.setLayout(noteId, layout);
}

std::string EngineImpl::transcribeSpan(whisper_context* ctx, const float* samples, std::size_t count,
                                       const std::string& language) {
    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.n_threads        = config_.threadCount > 0 ? config_.threadCount : 4;
    params.language         = language.c_str();
    params.translate        = false;
    params.no_timestamps    = true;
    params.single_segment   = true;
    params.print_progress   = false;
    params.print_realtime   = false;
    params.print_timestamps = false;
    params.print_special    = false;
    // whisper wants at least a second; a shorter turn is padded with silence.
    std::vector<float> padded(samples, samples + count);
    if (padded.size() < kSampleRate + kSampleRate / 10) { padded.resize(kSampleRate + kSampleRate / 10, 0.0F); }
    if (whisper_full(ctx, params, padded.data(), static_cast<int>(padded.size())) != 0) { return {}; }
    std::string text;
    for (int i = 0; i < whisper_full_n_segments(ctx); ++i) {
        const char* piece = whisper_full_get_segment_text(ctx, i);
        if (piece != nullptr) { text += piece; }
    }
    const auto first = text.find_first_not_of(" \t\n");
    if (first == std::string::npos) { return {}; }
    const auto last = text.find_last_not_of(" \t\n");
    return text.substr(first, last - first + 1);
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
    // The file before the row: a recording no note refers to is recovered as
    // a new note at the next launch, so a deleted one must not outlive its
    // row. If the row then fails to go, the sweep only forgets the file.
    const std::string name = store_.audioName(noteId);
    if (!name.empty() && !removeFile(recordingPath(name))) {
        return EngineStatus::FileRemovalFailed;
    }
    return store_.deleteNote(noteId, nullptr);
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

    std::vector<std::string> unreferenced;
    DIR* directory = ::opendir(config_.recordingsDirectory.c_str());
    if (directory == nullptr) { return; }
    while (const dirent* entry = ::readdir(directory)) {
        const std::string name = entry->d_name;
        if (isRecordingFileName(name) && referenced.count(name) == 0) {
            unreferenced.push_back(name);
        }
    }
    ::closedir(directory);

    for (const auto& name : unreferenced) { recoverRecording(name); }
}

void EngineImpl::recoverRecording(const std::string& name) {
    const std::string path = recordingPath(name);
    (void)finishWavHeader(path);
    std::int64_t durationMs = 0;
    {
        RecordingAudio audio;
        // Under a second is a tap on Record and nothing more, or not audio.
        if (!audio.open(path) || audio.count() < kSampleRate) {
            (void)removeFile(path);
            return;
        }
        durationMs = static_cast<std::int64_t>(audio.count() * 1000 / kSampleRate);
    }
    // No transcript yet and no language: "Improve transcript" makes both
    // from the audio. A failed insert leaves the file for the next sweep.
    (void)store_.insertNote("Recovered recording", recordingStartedAt(path, name), durationMs,
                            name, {}, {}, "auto");
}

std::int64_t EngineImpl::importRecording(const std::string& audioPath, const std::string& title,
                                         std::int64_t createdAt) {
    const std::string name = baseName(audioPath);
    if (!isRecordingFileName(name) || recordingPath(name) != audioPath) { return 0; }
    std::int64_t durationMs = 0;
    {
        RecordingAudio audio;
        if (!audio.open(audioPath)) { return 0; }
        durationMs = static_cast<std::int64_t>(audio.count() * 1000 / kSampleRate);
    }
    std::string language;
    {
        std::lock_guard<std::mutex> lock(languageMutex_);
        language = languagePreference_;
    }
    return store_.insertNote(title, createdAt, durationMs, name, {}, {}, language.empty() ? "auto" : language);
}

EngineStatus EngineImpl::writeBackup(const std::string& databasePath) {
    return store_.snapshot(databasePath);
}

RestoreResult EngineImpl::restoreBackup(const std::string& databasePath,
                                        const std::string& recordingsDirectory) {
    RestoreResult result;
    std::vector<NoteStore::RestoredNote> restored;
    result.status = store_.restoreFrom(databasePath, &restored, &result.skipped);
    if (result.status != EngineStatus::Ok) { return result; }
    result.restored = static_cast<std::int32_t>(restored.size());

    // Each recording under a name of its own: the backup's may be in use here.
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    for (const auto& note : restored) {
        if (note.audioName.empty() || note.audioName.find('/') != std::string::npos) { continue; }
        const std::string from = recordingsDirectory + "/" + note.audioName;
        if (!fileExists(from)) { continue; }
        const std::string name = "recording-" + std::to_string(stamp) + "-" + std::to_string(note.id) + ".wav";
        if (std::rename(from.c_str(), recordingPath(name).c_str()) != 0) { continue; }
        (void)store_.setAudio(note.id, name);
    }
    return result;
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
        case EngineStatus::TooLittleSpeech:    return "too little was said to summarise";
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
