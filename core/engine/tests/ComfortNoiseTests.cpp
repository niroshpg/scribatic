// =============================================================================
//  ComfortNoiseTests.cpp — the noise floor whisper decodes over digital silence.
//
//  Near-silent frames get noise at -60 dBFS, everything else stays
//  bit-identical, the same input always gives the same output, and restore()
//  gives back exactly what was there.
// =============================================================================
#include "ComfortNoise.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using scribatic::core::ComfortNoise;

namespace {

constexpr std::size_t kFrame = ComfortNoise::kFrame;

bool sameBits(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

double rms(const float* samples, std::size_t count) {
    double energy = 0.0;
    for (std::size_t i = 0; i < count; ++i) { energy += static_cast<double>(samples[i]) * samples[i]; }
    return std::sqrt(energy / static_cast<double>(count));
}

/// A tone at `level` RMS for frames [from, to), as a recording's speech.
void tone(std::vector<float>& samples, std::size_t from, std::size_t to, float level) {
    for (std::size_t i = from * kFrame; i < to * kFrame; ++i) {
        samples[i] = level * std::sqrt(2.0F) * std::sin(0.13F * static_cast<float>(i));
    }
}

/// Speech, then digital silence, then speech, then a quiet floor just
/// above the threshold, then one just below it, then a short last frame.
std::vector<float> recording() {
    std::vector<float> samples(kFrame * 400 + 100, 0.0F);
    tone(samples, 0, 50, 0.1F);             // speech, -20 dBFS
    // frames 50–149: exact zeros
    tone(samples, 150, 200, 0.05F);         // speech
    tone(samples, 200, 300, 0.0004F);       // -68 dBFS: a real room, untouched
    tone(samples, 300, 400, 0.0002F);       // -74 dBFS: near-silent, filled
    // the last 100 samples: zeros, a short frame
    samples[60 * kFrame + 7] = -0.0F;       // a negative zero to put back
    return samples;
}

void testFillsOnlyNearSilence() {
    const std::vector<float> original = recording();
    std::vector<float> samples = original;
    const ComfortNoise noise(samples.data(), samples.size(), false);
    assert(noise.framesFilled() == 100 + 100 + 1);

    for (std::size_t f = 0; f <= 400; ++f) {
        const std::size_t at = f * kFrame, length = std::min(kFrame, samples.size() - at);
        const bool filled = (f >= 50 && f < 150) || f >= 300;
        const bool same = std::memcmp(samples.data() + at, original.data() + at, length * sizeof(float)) == 0;
        assert(same != filled && "only near-silent frames change");
    }
    // The added noise, over the zeros: -60 dBFS within 1 dB.
    const double added = 20.0 * std::log10(rms(samples.data() + 50 * kFrame, 100 * kFrame));
    std::printf("  noise over silence: %.2f dBFS\n", added);
    assert(added > -61.0 && added < -59.0);
    // Each filled frame on its own, too: 320 samples of noise spread about
    // 0.35 dB, so a frame is held to 2 dB.
    for (std::size_t f = 50; f < 150; ++f) {
        const double level = 20.0 * std::log10(rms(samples.data() + f * kFrame, kFrame));
        assert(level > -62.0 && level < -58.0);
    }
    // Over the near-silent floor it is added, not swapped in.
    double difference = 0.0;
    for (std::size_t i = 300 * kFrame; i < 400 * kFrame; ++i) {
        const double d = static_cast<double>(samples[i]) - original[i];
        difference += d * d;
    }
    const double noiseLevel = 10.0 * std::log10(difference / (100.0 * kFrame));
    assert(noiseLevel > -61.0 && noiseLevel < -59.0);
}

void testDeterministic() {
    std::vector<float> a = recording(), b = recording();
    const ComfortNoise first(a.data(), a.size(), false);
    const ComfortNoise second(b.data(), b.size(), false);
    assert(sameBits(a, b) && "the same recording must always decode the same way");
}

void testRestoresExactly() {
    const std::vector<float> original = recording();
    std::vector<float> samples = original;
    {
        ComfortNoise noise(samples.data(), samples.size(), true);
        assert(!sameBits(samples, original));
        noise.restore();
        assert(sameBits(samples, original) && "restore() must give back every bit");
        noise.restore();   // twice is harmless
        assert(sameBits(samples, original));
    }
    {
        const ComfortNoise noise(samples.data(), samples.size(), true);
        assert(!sameBits(samples, original));
    }
    assert(sameBits(samples, original) && "the destructor restores too");
}

void testRealRecordingIsUntouched() {
    // A room's floor (-55 dBFS of noise) under speech: nothing to fill.
    std::vector<float> samples(16000 * 10);
    std::uint32_t seed = 12345;
    for (auto& sample : samples) {
        seed = seed * 1664525U + 1013904223U;
        sample = (static_cast<float>(seed >> 8) / 16777216.0F - 0.5F) * 0.006F;
    }
    const std::vector<float> original = samples;
    const ComfortNoise noise(samples.data(), samples.size(), true);
    assert(noise.framesFilled() == 0);
    assert(sameBits(samples, original));
}

void testEmpty() {
    const ComfortNoise none(nullptr, 0, true);
    assert(none.framesFilled() == 0);
    std::vector<float> tiny(10, 0.0F);   // one short frame
    const ComfortNoise one(tiny.data(), tiny.size(), false);
    assert(one.framesFilled() == 1);
}

} // namespace

int main() {
    std::printf("ComfortNoiseTests\n");
    testFillsOnlyNearSilence();
    testDeterministic();
    testRestoresExactly();
    testRealRecordingIsUntouched();
    testEmpty();
    std::printf("ok\n");
    return 0;
}
