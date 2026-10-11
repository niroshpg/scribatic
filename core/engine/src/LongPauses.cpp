#include "LongPauses.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace scribatic::core {

std::size_t LongPauses::cut(float* samples, std::size_t count) {
    pieces_.clear();
    cut_ = 0;
    if (samples == nullptr || count == 0) { return count; }
    const std::size_t frames = (count + kFrame - 1) / kFrame;
    std::vector<float> level(frames);
    for (std::size_t f = 0; f < frames; ++f) {
        const std::size_t at = f * kFrame, length = std::min(kFrame, count - at);
        double energy = 0.0;
        for (std::size_t i = at; i < at + length; ++i) { energy += static_cast<double>(samples[i]) * samples[i]; }
        level[f] = static_cast<float>(std::sqrt(energy / static_cast<double>(length)));
    }

    // The room's floor: the quietest 1% of frames that are not digital
    // silence. Even unbroken speech has that much between its words.
    std::vector<float> audible;
    for (const float l : level) { if (l >= kSilentRms) { audible.push_back(l); } }
    float pause = kSilentRms;
    if (!audible.empty()) {
        auto quietest = audible.begin() + static_cast<std::ptrdiff_t>(audible.size() / 100);
        std::nth_element(audible.begin(), quietest, audible.end());
        pause = std::clamp(*quietest * 2.0F, kSilentRms, kCeilingRms);   // +6 dB
    }

    // Kept stretches, as [from, to) in frames: everything but the middle of
    // each long pause.
    std::vector<std::pair<std::size_t, std::size_t>> kept;
    std::size_t from = 0;
    for (std::size_t f = 0; f < frames;) {
        if (level[f] >= pause) { ++f; continue; }
        std::size_t end = f;
        while (end < frames && level[end] < pause) { ++end; }
        if (end - f > kLongFrames) {
            kept.emplace_back(from, f + kKeepFrames);
            from = end - kKeepFrames;
        }
        f = end;
    }
    if (kept.empty()) { return count; }
    kept.emplace_back(from, frames);

    std::size_t to = 0;
    for (const auto& [first, last] : kept) {
        const std::size_t begin = first * kFrame, finish = std::min(count, last * kFrame);
        pieces_.push_back({to, begin});
        if (to != begin) { std::memmove(samples + to, samples + begin, (finish - begin) * sizeof(float)); }
        to += finish - begin;
    }
    cut_ = count - to;
    return to;
}

std::int64_t LongPauses::toOriginalMs(std::int64_t ms) const {
    if (pieces_.empty() || ms <= 0) { return ms; }
    const auto sample = static_cast<std::size_t>(ms) * kRate / 1000;
    auto piece = std::upper_bound(pieces_.begin(), pieces_.end(), sample,
                                  [](std::size_t s, const Piece& p) { return s < p.at; });
    --piece;   // the first piece starts at 0, so there is always one
    const std::size_t original = sample - piece->at + piece->original;
    return static_cast<std::int64_t>(original * 1000 / kRate);
}

} // namespace scribatic::core
