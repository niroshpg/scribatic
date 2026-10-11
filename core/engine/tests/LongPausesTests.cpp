// =============================================================================
//  LongPausesTests.cpp — long pauses cut before decoding, times mapped back.
//
//  Pauses over 2 s, in digital silence or a quiet room's floor, are shortened
//  to 1 s; speech, short pauses and quiet voices are left alone, sample for
//  sample; and a time in the shortened audio maps to the same moment in the
//  recording.
// =============================================================================
#include "LongPauses.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using scribatic::core::LongPauses;

namespace {

constexpr std::size_t kFrame  = LongPauses::kFrame;
constexpr std::size_t kSecond = 50;   // frames

/// White noise at `rms` over frames [from, to).
void noise(std::vector<float>& samples, std::size_t from, std::size_t to, float rms, std::uint32_t seed = 7) {
    const float width = rms * std::sqrt(3.0F) * 2.0F;
    for (std::size_t i = from * kFrame; i < to * kFrame; ++i) {
        seed = seed * 1664525U + 1013904223U;
        samples[i] += (static_cast<float>(seed >> 8) / 16777216.0F - 0.5F) * width;
    }
}

/// A tone at `rms` over frames [from, to), standing in for a voice.
void voice(std::vector<float>& samples, std::size_t from, std::size_t to, float rms) {
    for (std::size_t i = from * kFrame; i < to * kFrame; ++i) {
        samples[i] += rms * std::sqrt(2.0F) * std::sin(0.13F * static_cast<float>(i));
    }
}

bool same(const float* a, const float* b, std::size_t frames) {
    return std::memcmp(a, b, frames * kFrame * sizeof(float)) == 0;
}

void testDigitalSilenceIsShortened() {
    // 1 s of speech, 10 s of zeros, 1 s of speech.
    std::vector<float> samples((kSecond * 12) * kFrame, 0.0F);
    voice(samples, 0, kSecond, 0.05F);
    voice(samples, kSecond * 11, kSecond * 12, 0.05F);
    const std::vector<float> original = samples;
    LongPauses pauses;
    const std::size_t count = pauses.cut(samples.data(), samples.size());
    assert(count == kSecond * 3 * kFrame && "10 s of silence becomes 1 s");
    assert(pauses.samplesCut() == kSecond * 9 * kFrame);
    // Speech is kept sample for sample, and half a second of pause either side.
    assert(same(samples.data(), original.data(), kSecond + kSecond / 2));
    assert(same(samples.data() + (kSecond + kSecond / 2) * kFrame,
                original.data() + (kSecond * 10 + kSecond / 2) * kFrame, kSecond + kSecond / 2));
    // Times map back: before the cut unchanged, after it 9 s later.
    assert(pauses.toOriginalMs(0) == 0);
    assert(pauses.toOriginalMs(700) == 700);
    assert(pauses.toOriginalMs(2000) == 11000 && "the second sentence starts at 11 s in the recording");
    assert(pauses.toOriginalMs(2990) == 11990);
}

void testRoomFloorIsShortened() {
    // A phone in a quiet room: -65 dBFS of noise throughout, 20 s of it alone.
    std::vector<float> samples((kSecond * 24) * kFrame, 0.0F);
    noise(samples, 0, kSecond * 24, 0.00056F);
    voice(samples, 0, kSecond * 2, 0.05F);
    voice(samples, kSecond * 22, kSecond * 24, 0.05F);
    LongPauses pauses;
    const std::size_t count = pauses.cut(samples.data(), samples.size());
    assert(count == kSecond * 5 * kFrame && "20 s of room noise becomes 1 s");
    assert(pauses.toOriginalMs(3000) == 22000);
}

void testQuietVoiceIsKept() {
    // Room floor at -65 dBFS; someone across the room speaks for 10 s at
    // -50 dBFS between two near voices. That is speech, not a pause.
    std::vector<float> samples((kSecond * 14) * kFrame, 0.0F);
    noise(samples, 0, kSecond * 14, 0.00056F);
    voice(samples, 0, kSecond * 2, 0.05F);
    voice(samples, kSecond * 2, kSecond * 12, 0.00316F);
    voice(samples, kSecond * 12, kSecond * 14, 0.05F);
    const std::vector<float> original = samples;
    LongPauses pauses;
    assert(pauses.cut(samples.data(), samples.size()) == samples.size());
    assert(same(samples.data(), original.data(), kSecond * 14));
    assert(pauses.toOriginalMs(5000) == 5000);
}

void testShortPausesAreKept() {
    // Sentences with 0.3 s to 2 s between them, in digital silence.
    std::vector<float> samples((kSecond * 10 + 220) * kFrame, 0.0F);
    std::size_t at = 0;
    for (const std::size_t gap : {15, 30, 60, 100, 15}) {
        voice(samples, at, at + kSecond * 2, 0.05F);
        at += kSecond * 2 + gap;
    }
    const std::vector<float> original = samples;
    LongPauses pauses;
    assert(pauses.cut(samples.data(), samples.size()) == samples.size() && "a 2 s pause is not long");
    assert(same(samples.data(), original.data(), kSecond * 10 + 220));
}

void testNoisyRoomIsLeftAlone() {
    // A café: -45 dBFS of noise, louder than the ceiling. Nothing is cut,
    // even 10 s with no one speaking.
    std::vector<float> samples((kSecond * 14) * kFrame, 0.0F);
    noise(samples, 0, kSecond * 14, 0.0056F);
    voice(samples, 0, kSecond * 2, 0.1F);
    voice(samples, kSecond * 12, kSecond * 14, 0.1F);
    LongPauses pauses;
    assert(pauses.cut(samples.data(), samples.size()) == samples.size());
}

void testPausesAtTheEnds() {
    // 5 s of zeros, 1 s of speech, 5 s of zeros, and a short last frame.
    std::vector<float> samples((kSecond * 11) * kFrame + 100, 0.0F);
    voice(samples, kSecond * 5, kSecond * 6, 0.05F);
    LongPauses pauses;
    const std::size_t count = pauses.cut(samples.data(), samples.size());
    // Half a second kept at the start, either side of the speech, and at the end.
    assert(count == (kSecond * 3 - 1) * kFrame + 100);
    assert(pauses.toOriginalMs(400) == 400);
    assert(pauses.toOriginalMs(1000) == 5000 && "the speech starts 5 s in");
    assert(pauses.toOriginalMs(1999) == 5999);
    assert(pauses.toOriginalMs(2500) == 10520);   // the last 0.5 s and the short frame
}

void testEmpty() {
    LongPauses pauses;
    assert(pauses.cut(nullptr, 0) == 0);
    std::vector<float> silence(kSecond * kFrame, 0.0F);   // 1 s: too short to cut
    assert(pauses.cut(silence.data(), silence.size()) == silence.size());
    assert(pauses.toOriginalMs(400) == 400);
}

} // namespace

int main() {
    std::printf("LongPausesTests\n");
    testDigitalSilenceIsShortened();
    testRoomFloorIsShortened();
    testQuietVoiceIsKept();
    testShortPausesAreKept();
    testNoisyRoomIsLeftAlone();
    testPausesAtTheEnds();
    testEmpty();
    std::printf("ok\n");
    return 0;
}
