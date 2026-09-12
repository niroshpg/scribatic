#include "scribatic/core/AudioRingBuffer.hpp"

#include <algorithm>
#include <cstring>

namespace scribatic::core {
namespace {

std::size_t roundUpToPowerOfTwo(std::size_t value) noexcept {
    std::size_t result = 1;
    while (result < value) {
        result <<= 1U;
    }
    return result;
}

} // namespace

AudioRingBuffer::AudioRingBuffer(std::size_t requestedFrames)
    : capacity_(roundUpToPowerOfTwo(std::max<std::size_t>(requestedFrames, 2))) {
    mask_   = capacity_ - 1;
    buffer_ = std::make_unique<float[]>(capacity_);  // the only allocation, ever
}

std::size_t AudioRingBuffer::write(const float* src, std::size_t frames) noexcept {
    const std::size_t head = writeIndex_.load(std::memory_order_relaxed);
    const std::size_t tail = readIndex_.load(std::memory_order_acquire);
    const std::size_t free = capacity_ - (head - tail) - 1;
    const std::size_t n    = std::min(frames, free);

    for (std::size_t i = 0; i < n; ++i) {
        buffer_[(head + i) & mask_] = src[i];
    }
    writeIndex_.store(head + n, std::memory_order_release);
    return n;
}

std::size_t AudioRingBuffer::read(float* dst, std::size_t frames) noexcept {
    const std::size_t tail  = readIndex_.load(std::memory_order_relaxed);
    const std::size_t head  = writeIndex_.load(std::memory_order_acquire);
    const std::size_t ready = head - tail;
    const std::size_t n     = std::min(frames, ready);

    for (std::size_t i = 0; i < n; ++i) {
        dst[i] = buffer_[(tail + i) & mask_];
    }
    readIndex_.store(tail + n, std::memory_order_release);
    return n;
}

std::size_t AudioRingBuffer::available() const noexcept {
    return writeIndex_.load(std::memory_order_acquire) -
           readIndex_.load(std::memory_order_acquire);
}

void AudioRingBuffer::reset() noexcept {
    readIndex_.store(0, std::memory_order_relaxed);
    writeIndex_.store(0, std::memory_order_relaxed);
}

} // namespace scribatic::core
