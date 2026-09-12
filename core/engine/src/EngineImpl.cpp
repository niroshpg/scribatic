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

void scribaticEngineRetain(EngineInterface* engine) noexcept {
    if (auto* impl = static_cast<EngineImpl*>(engine)) { impl->retain(); }
}

void scribaticEngineRelease(EngineInterface* engine) noexcept {
    if (auto* impl = static_cast<EngineImpl*>(engine)) { impl->release(); }
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

    // TODO(backend): whisper_init_from_buffer() / llama_model_load_from_buffer()
    // over the mapped regions, then prime the KV cache with a silent frame so
    // the first user utterance does not pay the graph-allocation cost.

    state_.store(EngineState::Listening, std::memory_order_release);
    return EngineStatus::Ok;
}

void EngineImpl::hibernate() noexcept {
    // TODO(backend): free ggml contexts before unmapping the weights.
    llamaWeights_.unmap();
    whisperWeights_.unmap();
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

    // TODO(backend): whisper_full() with an abort callback observing
    // cancelRequested_ between graph nodes, then append finalised segments:
    //   std::lock_guard<std::mutex> lock(segmentMutex_);
    //   pendingSegments_.push_back(...);

    state_.store(EngineState::Listening, std::memory_order_release);
    return cancelRequested_.load(std::memory_order_acquire) ? EngineStatus::Cancelled
                                                            : EngineStatus::Ok;
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
