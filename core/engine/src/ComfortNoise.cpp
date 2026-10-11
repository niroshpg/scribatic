#include "ComfortNoise.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace scribatic::core {
namespace {

/// xorshift64* and Box-Muller: not std::normal_distribution, whose output
/// differs between standard libraries, so the same recording decodes the
/// same way on every platform and in the tests.
class Gaussian {
public:
    float next() {
        if (spare_) {
            spare_ = false;
            return second_;
        }
        const double u1 = uniform(), u2 = uniform();
        const double radius = std::sqrt(-2.0 * std::log(u1));
        constexpr double kTwoPi = 6.283185307179586;
        second_ = static_cast<float>(radius * std::sin(kTwoPi * u2));
        spare_  = true;
        return static_cast<float>(radius * std::cos(kTwoPi * u2));
    }

private:
    double uniform() {   // (0, 1]: never 0, whose log is -inf
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        const std::uint64_t bits = (state_ * 0x2545F4914F6CDD1DULL) >> 11;
        return (static_cast<double>(bits) + 1.0) / 9007199254740992.0;   // 2^53
    }

    std::uint64_t state_  = 0x9E3779B97F4A7C15ULL;
    bool          spare_  = false;
    float         second_ = 0.0F;
};

bool allZeroBits(const float* samples, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, samples + i, sizeof bits);
        if (bits != 0) { return false; }
    }
    return true;
}

} // namespace

ComfortNoise::ComfortNoise(float* samples, std::size_t count, bool restorable)
    : samples_(restorable ? samples : nullptr) {
    if (samples == nullptr) { return; }
    constexpr double kBelowEnergy = static_cast<double>(kBelowRms) * kBelowRms;
    Gaussian noise;
    for (std::size_t at = 0; at < count; at += kFrame) {
        const std::size_t length = std::min(kFrame, count - at);
        float* frame = samples + at;
        double energy = 0.0;
        for (std::size_t i = 0; i < length; ++i) { energy += static_cast<double>(frame[i]) * frame[i]; }
        if (energy >= kBelowEnergy * static_cast<double>(length)) { continue; }
        if (restorable) {
            if (allZeroBits(frame, length)) {
                undo_.push_back({at, length, kZeros});
            } else {
                undo_.push_back({at, length, saved_.size()});
                saved_.insert(saved_.end(), frame, frame + length);
            }
        }
        for (std::size_t i = 0; i < length; ++i) { frame[i] += kNoiseRms * noise.next(); }
        ++filled_;
    }
}

void ComfortNoise::restore() noexcept {
    if (samples_ == nullptr) { return; }
    for (const auto& frame : undo_) {
        if (frame.saved == kZeros) {
            std::fill_n(samples_ + frame.at, frame.length, 0.0F);
        } else {
            std::copy_n(saved_.data() + frame.saved, frame.length, samples_ + frame.at);
        }
    }
    undo_.clear();
    saved_.clear();
    samples_ = nullptr;
}

} // namespace scribatic::core
