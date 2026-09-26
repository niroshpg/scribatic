// =============================================================================
//  RecordingAudio.hpp — read access to a recording the platform wrote.
//
//  Both apps write 16 kHz mono WAV. Diarization needs the whole recording as
//  one float array, and an hour of it is 230 MB; reading that onto the heap
//  would be the dirty-anonymous-memory pattern ADR-004 exists to prevent. So a
//  float32 file is mapped, and the samples are handed over straight from the
//  page cache — clean, file-backed, evictable.
//
//  16-bit PCM is accepted too, for test fixtures, at the cost of a converted
//  copy. The apps never write it.
// =============================================================================
#pragma once

#include "scribatic/core/ModelResidency.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace scribatic::core {

class RecordingAudio {
public:
    /// False if the file is missing, not a WAV, or not mono 16 kHz.
    bool open(const std::string& path);

    [[nodiscard]] const float* samples() const { return samples_; }
    [[nodiscard]] std::size_t  count() const { return count_; }

private:
    ModelResidency     mapping_;
    std::vector<float> converted_;   ///< only for non-float or misaligned data
    const float*       samples_ = nullptr;
    std::size_t        count_   = 0;
};

} // namespace scribatic::core
