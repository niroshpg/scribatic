// =============================================================================
//  ComfortNoise.hpp — a noise floor for whisper where the audio has none.
//
//  Whisper Small, decoding greedily with timestamps, drops the first word of a
//  sentence that starts straight out of digital silence (exact zeros): "For
//  next week, please read…" came back as "next week, please read…". Any
//  realistic noise floor in the pause brings the word back. Microphone
//  recordings always have one (the quietest frame measured was -66 dBFS);
//  imported files (a muted meeting participant, an edited podcast, text to
//  speech), gated Bluetooth microphones and zero-padded short turns may not.
//
//  So every 20 ms frame quieter than -70 dBFS gets Gaussian noise at -60 dBFS
//  added. Every other sample is left bit-identical, which makes this a no-op on
//  every realistic recording measured (claude-notes/dropped-word/PROPOSAL.md).
//
//  Only what whisper decodes is filled. The energy checks that drop phrases
//  whisper invents over silence compare against kSilenceRms (also -60 dBFS),
//  so they must read the original samples: fill in place, decode, restore.
//  The noise is deterministic, so a recording always decodes the same way.
//
//  Long pauses are shortened first (LongPauses): over 10–60 s of silence,
//  filled or not, whisper drops or invents whole windows. Filling only the
//  edges of a long silence, or only short pauses, lost more (2026-10-11).
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace scribatic::core {

class ComfortNoise {
public:
    static constexpr std::size_t kFrame    = 320;          ///< 20 ms at 16 kHz
    static constexpr float       kBelowRms = 0.000316F;    ///< -70 dBFS
    static constexpr float       kNoiseRms = 0.001F;       ///< -60 dBFS

    /// Fills `samples` in place. With `restorable`, keeps what it overwrote
    /// so that restore(), or the destructor, puts the original back exactly.
    ComfortNoise(float* samples, std::size_t count, bool restorable);
    ~ComfortNoise() { restore(); }
    ComfortNoise(const ComfortNoise&)            = delete;
    ComfortNoise& operator=(const ComfortNoise&) = delete;

    void restore() noexcept;

    [[nodiscard]] std::size_t framesFilled() const noexcept { return filled_; }

private:
    struct Overwritten {
        std::size_t at;       ///< first sample of the frame
        std::size_t length;   ///< the last frame may be short
        std::size_t saved;    ///< offset into saved_, or kZeros
    };
    static constexpr std::size_t kZeros = static_cast<std::size_t>(-1);

    float*                   samples_ = nullptr;
    std::size_t              filled_  = 0;
    std::vector<Overwritten> undo_;
    std::vector<float>       saved_;   ///< only frames that were not exact zeros
};

} // namespace scribatic::core
