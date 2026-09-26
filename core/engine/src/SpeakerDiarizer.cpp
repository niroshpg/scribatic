#include "SpeakerDiarizer.hpp"

#if defined(SCRIBATIC_HAVE_SHERPA)
#include "sherpa-onnx/c-api/c-api.h"
#endif

#include <cstring>
#include <limits>

namespace scribatic::core {

#if defined(SCRIBATIC_HAVE_SHERPA)
namespace {

/// Cosine-distance threshold for estimating the speaker count, chosen by
/// measurement (ADR-009). At 0.85 a four-person discussion came back as three
/// people — two voices merged, which nobody can undo afterwards. At 0.7 every
/// voice stayed separate, at the cost of sometimes splitting one person in
/// two, which the user repairs by giving both labels the same name.
constexpr float kEstimateThreshold = 0.7F;

/// Speech shorter than this is not given a turn of its own, and a pause
/// shorter than this does not end one. sherpa-onnx's documented defaults.
constexpr float kMinDurationOn  = 0.3F;
constexpr float kMinDurationOff = 0.5F;

SherpaOnnxOfflineSpeakerDiarizationConfig makeConfig(const std::string& segmentationModel,
                                                     const std::string& embeddingModel,
                                                     std::int32_t threads,
                                                     std::int32_t expectedSpeakers) {
    SherpaOnnxOfflineSpeakerDiarizationConfig config;
    std::memset(&config, 0, sizeof config);
    config.segmentation.pyannote.model = segmentationModel.c_str();
    config.segmentation.num_threads    = threads;
    config.segmentation.provider       = "cpu";
    config.embedding.model             = embeddingModel.c_str();
    config.embedding.num_threads       = threads;
    config.embedding.provider          = "cpu";
    config.clustering.num_clusters     = expectedSpeakers > 0 ? expectedSpeakers : 0;
    config.clustering.threshold        = kEstimateThreshold;
    config.min_duration_on             = kMinDurationOn;
    config.min_duration_off            = kMinDurationOff;
    return config;
}

const SherpaOnnxOfflineSpeakerDiarization* asSherpa(const void* handle) {
    return static_cast<const SherpaOnnxOfflineSpeakerDiarization*>(handle);
}

} // namespace

bool SpeakerDiarizer::compiledIn() noexcept { return true; }

std::vector<SpeakerTurn> SpeakerDiarizer::run(const std::string& segmentationModel,
                                              const std::string& embeddingModel,
                                              std::int32_t threads,
                                              const float* samples, std::size_t count,
                                              std::int32_t expectedSpeakers,
                                              EngineStatus* outStatus) {
    const auto fail = [outStatus](EngineStatus status) {
        if (outStatus != nullptr) { *outStatus = status; }
        return std::vector<SpeakerTurn>{};
    };

    std::lock_guard<std::mutex> lock(mutex_);
    const auto config = makeConfig(segmentationModel, embeddingModel,
                                   threads > 0 ? threads : 2, expectedSpeakers);
    if (handle_ == nullptr) {
        handle_ = SherpaOnnxCreateOfflineSpeakerDiarization(&config);
        if (handle_ == nullptr) { return fail(EngineStatus::ModelLoadFailed); }
    } else {
        // Only the clustering half of the config is applied here, which is
        // the only half that differs between calls.
        SherpaOnnxOfflineSpeakerDiarizationSetConfig(asSherpa(handle_), &config);
    }

    if (count > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        return fail(EngineStatus::UnsupportedFormat);   // ~37 hours; not a real recording
    }

    const auto* result = SherpaOnnxOfflineSpeakerDiarizationProcess(
        asSherpa(handle_), samples, static_cast<std::int32_t>(count));
    if (result == nullptr) { return fail(EngineStatus::InferenceFailed); }

    const std::int32_t n = SherpaOnnxOfflineSpeakerDiarizationResultGetNumSegments(result);
    const auto* segments = SherpaOnnxOfflineSpeakerDiarizationResultSortByStartTime(result);

    std::vector<SpeakerTurn> turns;
    turns.reserve(static_cast<std::size_t>(n > 0 ? n : 0));
    for (std::int32_t i = 0; segments != nullptr && i < n; ++i) {
        SpeakerTurn turn;
        turn.startMs = static_cast<std::int64_t>(segments[i].start * 1000.0F);
        turn.endMs   = static_cast<std::int64_t>(segments[i].end * 1000.0F);
        turn.speaker = segments[i].speaker;
        turns.push_back(turn);
    }

    if (segments != nullptr) { SherpaOnnxOfflineSpeakerDiarizationDestroySegment(segments); }
    SherpaOnnxOfflineSpeakerDiarizationDestroyResult(result);

    if (outStatus != nullptr) { *outStatus = EngineStatus::Ok; }
    return turns;
}

void SpeakerDiarizer::unload() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (handle_ != nullptr) {
        SherpaOnnxDestroyOfflineSpeakerDiarization(asSherpa(handle_));
        handle_ = nullptr;
    }
}

#else  // !SCRIBATIC_HAVE_SHERPA

bool SpeakerDiarizer::compiledIn() noexcept { return false; }

std::vector<SpeakerTurn> SpeakerDiarizer::run(const std::string&, const std::string&,
                                              std::int32_t, const float*, std::size_t,
                                              std::int32_t, EngineStatus* outStatus) {
    if (outStatus != nullptr) { *outStatus = EngineStatus::SpeakersUnavailable; }
    return {};
}

void SpeakerDiarizer::unload() noexcept {}

#endif

SpeakerDiarizer::~SpeakerDiarizer() { unload(); }

} // namespace scribatic::core
