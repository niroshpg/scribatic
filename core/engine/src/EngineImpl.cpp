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

#include "whisper.h"

#include <sys/stat.h>

#include <utility>

namespace scribatic::core {
namespace {

bool fileExists(const std::string& path) noexcept {
    struct stat info {};
    return !path.empty() && ::stat(path.c_str(), &info) == 0;
}

} // namespace

// -- Factory & lifetime -------------------------------------------------------

EngineInterface* EngineInterface::create(const EngineConfig& config,
                                         EngineStatus* outStatus) noexcept {
    const auto fail = [outStatus](EngineStatus status) -> EngineInterface* {
        if (outStatus != nullptr) { *outStatus = status; }
        return nullptr;
    };

    if (!fileExists(config.whisperModelPath) || !fileExists(config.llamaModelPath)) {
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

    if (config_.useMemoryMapping) {
        if (!whisperWeights_.map(config_.whisperModelPath) ||
            !llamaWeights_.map(config_.llamaModelPath)) {
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

    // TODO(backend): llama_model_load_from_buffer() over llamaWeights_.

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

    // TODO(backend): free the llama context here, before its weights unmap.
    llamaWeights_.unmap();
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

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.n_threads        = config_.threadCount > 0 ? config_.threadCount : 4;
    params.translate        = false;
    params.language         = "en";
    params.no_timestamps    = false;
    // Nothing is allowed to write to stdout from inside a mobile app.
    params.print_progress   = false;
    params.print_realtime   = false;
    params.print_timestamps = false;
    params.print_special    = false;
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
    std::vector<TranscriptSegment> decoded;
    decoded.reserve(static_cast<std::size_t>(count));

    for (int i = 0; i < count; ++i) {
        const char* text = whisper_full_get_segment_text(whisper_, i);
        if (text == nullptr) { continue; }

        std::string trimmed(text);
        const auto first = trimmed.find_first_not_of(" \t\n");
        if (first == std::string::npos) { continue; }   // silence decodes to blanks
        trimmed.erase(0, first);

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

        decoded.push_back(std::move(segment));
    }

    decodedSamples_ += static_cast<std::int64_t>(window_.size());
    window_.clear();

    if (!decoded.empty()) {
        std::lock_guard<std::mutex> lock(segmentMutex_);
        for (auto& segment : decoded) {
            pendingSegments_.push_back(std::move(segment));
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

std::string EngineImpl::summarize(const std::string& transcript) {
    state_.store(EngineState::Summarizing, std::memory_order_release);
    // TODO(backend): llama_decode() loop over the local insight prompt template.
    (void)transcript;
    state_.store(EngineState::Listening, std::memory_order_release);
    return {};
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
