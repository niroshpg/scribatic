// =============================================================================
//  LongPauses.hpp — long pauses cut out before the accurate pass decodes.
//
//  Whisper decodes in 30 s windows, and a window that is mostly pause can
//  lose the speech after it: with 10–60 s pauses between sentences, whole
//  sentences went missing (12 of 112 in synthetic speech, 11 of 13 in one
//  clip) and whisper looped on invented lines, whether the pauses held exact
//  zeros or a quiet room's noise. With every pause over 2 s shortened to 1 s,
//  none were lost, and human speech lost fewer words (2026-10-11).
//
//  A pause is 2 s or more in which every 20 ms frame is near-silent or within
//  6 dB of the recording's own noise floor, which is never taken above
//  -55 dBFS: a quiet voice across a room (-50 dBFS) is speech, not a pause.
//  Half a second of each pause is kept on either side, so a sentence still
//  starts out of a pause, as whisper expects.
//
//  The samples are moved in place: an hour is 230 MB, and on a 4 GB device a
//  copy is not to be had. Times decoded from the shortened audio map back to
//  the recording's with toOriginalMs().
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace scribatic::core {

class LongPauses {
public:
    static constexpr std::size_t kRate       = 16000;
    static constexpr std::size_t kFrame      = 320;         ///< 20 ms
    static constexpr std::size_t kLongFrames = 100;         ///< over 2 s is long
    static constexpr std::size_t kKeepFrames = 25;          ///< 0.5 s kept each side
    static constexpr float       kSilentRms  = 0.000316F;   ///< -70 dBFS: silent whatever the floor
    static constexpr float       kCeilingRms = 0.00178F;    ///< -55 dBFS: never a pause above this

    /// Cuts the long pauses out of `samples`, moving the rest down. Returns
    /// how many samples are left.
    std::size_t cut(float* samples, std::size_t count);

    /// A time in the shortened audio, in the recording.
    [[nodiscard]] std::int64_t toOriginalMs(std::int64_t ms) const;

    [[nodiscard]] std::size_t samplesCut() const noexcept { return cut_; }

private:
    struct Piece {
        std::size_t at;         ///< first sample, shortened
        std::size_t original;   ///< the same sample, in the recording
    };
    std::vector<Piece> pieces_;
    std::size_t        cut_ = 0;
};

} // namespace scribatic::core
