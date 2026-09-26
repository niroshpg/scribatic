// =============================================================================
//  SpeakerDiarizer.hpp — "who spoke when", via sherpa-onnx (ADR-009).
//
//  Two ONNX models: pyannote segmentation finds speech and voice changes, and a
//  speaker-embedding model turns each stretch into a vector that is clustered
//  into speakers. The embeddings live only for the duration of `run()`; none is
//  returned, stored, or compared across recordings.
//
//  Compiled to a stub when sherpa-onnx is not linked (SCRIBATIC_HAVE_SHERPA
//  undefined): the engine still transcribes, and reports speaker
//  identification as unavailable instead of failing to build.
// =============================================================================
#pragma once

#include "SpeakerAttribution.hpp"
#include "scribatic/core/Types.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace scribatic::core {

class SpeakerDiarizer {
public:
    SpeakerDiarizer() = default;
    ~SpeakerDiarizer();
    SpeakerDiarizer(const SpeakerDiarizer&) = delete;
    SpeakerDiarizer& operator=(const SpeakerDiarizer&) = delete;

    /// Whether this build links the diarization library at all.
    [[nodiscard]] static bool compiledIn() noexcept;

    /// Diarizes 16 kHz mono samples. Models are loaded on first use and kept
    /// until `unload()`. `expectedSpeakers` <= 0 estimates the count.
    std::vector<SpeakerTurn> run(const std::string& segmentationModel,
                                 const std::string& embeddingModel,
                                 std::int32_t threads,
                                 const float* samples, std::size_t count,
                                 std::int32_t expectedSpeakers,
                                 EngineStatus* outStatus);

    void unload() noexcept;

private:
    std::mutex  mutex_;
    const void* handle_ = nullptr;   // SherpaOnnxOfflineSpeakerDiarization*
};

} // namespace scribatic::core
