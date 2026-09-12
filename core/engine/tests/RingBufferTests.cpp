// =============================================================================
//  RingBufferTests.cpp — dependency-free assertions for the realtime FIFO.
//
//  No gtest: the core must build and self-test on a bare toolchain (CI image,
//  NDK sysroot) without fetching a test framework.
// =============================================================================
#include "scribatic/core/AudioRingBuffer.hpp"
#include "scribatic/core/EngineInterface.hpp"

#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

using scribatic::core::AudioRingBuffer;

namespace {

void testWriteReadRoundTrip() {
    AudioRingBuffer ring(1024);
    std::vector<float> in(512);
    for (std::size_t i = 0; i < in.size(); ++i) {
        in[i] = static_cast<float>(i);
    }

    assert(ring.write(in.data(), in.size()) == in.size());
    assert(ring.available() == in.size());

    std::vector<float> out(512, -1.0F);
    assert(ring.read(out.data(), out.size()) == out.size());
    assert(out == in);
    assert(ring.available() == 0);
}

void testOverrunIsReportedNotBlocked() {
    AudioRingBuffer ring(64);                 // rounds to 64, usable 63
    std::vector<float> in(256, 1.0F);
    const std::size_t written = ring.write(in.data(), in.size());
    assert(written < in.size());              // short write, never a block
}

void testWrapAround() {
    AudioRingBuffer ring(8);
    std::vector<float> chunk(4, 2.0F);
    std::vector<float> out(4, 0.0F);
    for (int cycle = 0; cycle < 16; ++cycle) {
        assert(ring.write(chunk.data(), chunk.size()) == chunk.size());
        assert(ring.read(out.data(), out.size()) == out.size());
        assert(out[0] == 2.0F && out[3] == 2.0F);
    }
}

/// Producer and consumer on separate threads: no torn reads, no lost frames.
void testSingleProducerSingleConsumer() {
    constexpr std::size_t kTotal = 200000;
    AudioRingBuffer ring(4096);

    std::thread producer([&] {
        std::size_t sent = 0;
        float sample = 1.0F;
        while (sent < kTotal) {
            sent += ring.write(&sample, 1);
        }
    });

    std::size_t received = 0;
    float value = 0.0F;
    while (received < kTotal) {
        received += ring.read(&value, 1);
    }
    producer.join();
    assert(received == kTotal);
}

void testStatusDescriptionsAreTotal() {
    using scribatic::core::EngineStatus;
    for (int code = 0; code <= 9; ++code) {
        const auto text = scribatic::core::describeStatus(static_cast<EngineStatus>(code));
        assert(!text.empty() && text != "unknown");
    }
}

} // namespace

int main() {
    testWriteReadRoundTrip();
    testOverrunIsReportedNotBlocked();
    testWrapAround();
    testSingleProducerSingleConsumer();
    testStatusDescriptionsAreTotal();
    std::printf("scribatic_core: all tests passed\n");
    return 0;
}
