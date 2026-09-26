#include "RecordingAudio.hpp"

#include <sys/mman.h>

#include <cstdint>
#include <cstring>

namespace scribatic::core {
namespace {

constexpr std::uint16_t kFormatPcm        = 1;
constexpr std::uint16_t kFormatFloat      = 3;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;

std::uint16_t u16(const unsigned char* p) {
    std::uint16_t v = 0;
    std::memcpy(&v, p, 2);
    return v;
}

std::uint32_t u32(const unsigned char* p) {
    std::uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

} // namespace

bool RecordingAudio::open(const std::string& path) {
    samples_ = nullptr;
    count_   = 0;
    converted_.clear();

    if (!mapping_.map(path)) { return false; }
    const auto* bytes = static_cast<const unsigned char*>(mapping_.data());
    const std::size_t size = mapping_.size();
    if (size < 12 || std::memcmp(bytes, "RIFF", 4) != 0 || std::memcmp(bytes + 8, "WAVE", 4) != 0) {
        return false;
    }

    // Diarization reads front to back exactly once. ModelResidency advises
    // random access because that is how transformer weights are walked; a
    // recording is the opposite, and readahead is exactly what it wants.
    (void)::madvise(const_cast<void*>(mapping_.data()), size, MADV_SEQUENTIAL);

    std::uint16_t format = 0, channels = 0, bits = 0;
    std::uint32_t rate = 0;

    // Walk the chunk list. AVAudioFile writes FLLR padding before `data`, so
    // the canonical 44-byte header is not something to rely on.
    std::size_t pos = 12;
    while (pos + 8 <= size) {
        const unsigned char* chunk = bytes + pos;
        const std::uint32_t chunkSize = u32(chunk + 4);
        const std::size_t body = pos + 8;

        if (std::memcmp(chunk, "fmt ", 4) == 0 && body + 16 <= size) {
            format   = u16(bytes + body);
            channels = u16(bytes + body + 2);
            rate     = u32(bytes + body + 4);
            bits     = u16(bytes + body + 14);
            if (format == kFormatExtensible && chunkSize >= 40 && body + 26 <= size) {
                format = u16(bytes + body + 24);    // first two bytes of the subformat GUID
            }
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            if (channels != 1 || rate != 16000) { return false; }

            // A header whose size was never patched — the app was killed
            // mid-recording — reads 0 or runs past the end. Either way the
            // samples are simply everything that made it to disk.
            std::size_t dataBytes = size - body;
            if (chunkSize != 0 && chunkSize <= dataBytes) { dataBytes = chunkSize; }

            if (format == kFormatFloat && bits == 32) {
                count_ = dataBytes / 4;
                if (body % alignof(float) == 0) {
                    samples_ = reinterpret_cast<const float*>(bytes + body);
                } else {
                    converted_.resize(count_);
                    std::memcpy(converted_.data(), bytes + body, count_ * 4);
                    samples_ = converted_.data();
                }
            } else if (format == kFormatPcm && bits == 16) {
                count_ = dataBytes / 2;
                converted_.resize(count_);
                for (std::size_t i = 0; i < count_; ++i) {
                    std::int16_t sample = 0;
                    std::memcpy(&sample, bytes + body + i * 2, 2);
                    converted_[i] = static_cast<float>(sample) / 32768.0F;
                }
                samples_ = converted_.data();
            } else {
                return false;
            }
            return count_ > 0;
        }
        pos = body + chunkSize + (chunkSize & 1);   // chunks are word-aligned
    }
    return false;
}

} // namespace scribatic::core
