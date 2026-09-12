// =============================================================================
//  AudioRingBuffer.hpp — Single-producer / single-consumer lock-free FIFO.
//
//  Producer: the OS audio callback thread (AVAudioEngine tap on iOS, AAudio
//  high-priority callback on Android). That thread runs under a hard deadline;
//  a malloc, a mutex or a page fault inside it produces an audible glitch.
//  Consumer: the engine's inference thread.
//
//  Capacity is rounded up to a power of two so the modulo reduces to a mask.
//  Storage is allocated exactly once, at construction, outside the audio path.
// =============================================================================
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <new>

namespace scribatic::core {

class AudioRingBuffer {
public:
    explicit AudioRingBuffer(std::size_t requestedFrames);

    /// Realtime-safe. Returns the number of frames actually written; a short
    /// write means the consumer stalled and the caller should drop the rest.
    std::size_t write(const float* src, std::size_t frames) noexcept;

    /// Called from the inference thread. Returns frames actually read.
    std::size_t read(float* dst, std::size_t frames) noexcept;

    [[nodiscard]] std::size_t available() const noexcept;
    void reset() noexcept;

private:
    static constexpr std::size_t kCacheLine = 64;

    std::unique_ptr<float[]> buffer_;
    std::size_t              capacity_ = 0;   ///< always a power of two
    std::size_t              mask_     = 0;

    // Head and tail live on separate cache lines to avoid false sharing
    // between the audio thread and the inference thread.
    alignas(kCacheLine) std::atomic<std::size_t> writeIndex_{0};
    alignas(kCacheLine) std::atomic<std::size_t> readIndex_{0};
};

} // namespace scribatic::core
