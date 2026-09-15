// =============================================================================
//  TranscriptionTests.cpp — end-to-end assertion that audio in becomes text out.
//
//  This is the test the TODO(backend) work existed to make possible: it drives
//  the real public interface, with the real weights, over real recorded speech,
//  and asserts the words come back.
//
//  It SKIPS rather than fails when the weights are absent. They are hundreds of
//  megabytes and deliberately uncommitted, so CI and a fresh clone have none —
//  and a test that cannot run is not the same thing as a test that failed.
// =============================================================================
#include "scribatic/core/EngineInterface.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using scribatic::core::EngineConfig;
using scribatic::core::EngineInterface;
using scribatic::core::EngineStatus;

namespace {

bool fileExists(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

/// Minimal 16-bit PCM WAV reader. Enough for the fixture and nothing more:
/// pulling in a parsing library would breach the dependency-free rule the rest
/// of this suite is built on.
bool readWav16(const std::string& path, std::vector<float>& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) { return false; }

    std::vector<char> bytes((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
    if (bytes.size() < 44) { return false; }

    // Walk the chunk list rather than assuming a 44-byte header: afconvert
    // emits a FLLR padding chunk before `data`, so a fixed offset reads padding
    // as samples and transcribes to silence.
    std::size_t pos = 12;                       // past "RIFF....WAVE"
    while (pos + 8 <= bytes.size()) {
        const std::string id(bytes.data() + pos, 4);
        std::uint32_t size = 0;
        std::memcpy(&size, bytes.data() + pos + 4, 4);

        if (id == "data") {
            const std::size_t start = pos + 8;
            const std::size_t count = std::min<std::size_t>(size, bytes.size() - start) / 2;
            out.resize(count);
            for (std::size_t i = 0; i < count; ++i) {
                std::int16_t sample = 0;
                std::memcpy(&sample, bytes.data() + start + i * 2, 2);
                out[i] = static_cast<float>(sample) / 32768.0F;
            }
            return count > 0;
        }
        pos += 8 + size + (size & 1);           // chunks are word-aligned
    }
    return false;
}

std::string lowercased(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string repoPath(const char* relative) {
    const char* root = std::getenv("SCRIBATIC_ROOT");
    return std::string(root != nullptr ? root : "../..") + "/" + relative;
}

void testSpeechBecomesTranscript() {
    const std::string whisperModel = repoPath("models/ggml-base.en.bin");
    const std::string llamaModel   = repoPath("models/insight-q4_k_m.gguf");
    const std::string fixture      = repoPath("core/engine/tests/fixtures/speech-16k-mono.wav");

    if (!fileExists(whisperModel)) {
        std::printf("  skip: no weights at %s (run `make fetch-models`)\n", whisperModel.c_str());
        return;
    }
    if (!fileExists(fixture)) {
        std::printf("  skip: no fixture at %s\n", fixture.c_str());
        return;
    }

    std::vector<float> pcm;
    assert(readWav16(fixture, pcm) && "fixture should decode");
    assert(pcm.size() > 16000 && "fixture should be over a second of audio");

    EngineConfig config;
    config.whisperModelPath = whisperModel;
    config.llamaModelPath   = llamaModel;
    config.databasePath     = "/tmp/scribatic-test.sqlite";
    config.threadCount      = 4;
    config.useMemoryMapping = true;

    EngineStatus status = EngineStatus::Ok;
    EngineInterface* engine = EngineInterface::create(config, &status);
    if (engine == nullptr) {
        // create() requires BOTH model paths to exist. The llama weights have
        // no published URL yet, so a missing one is an environment gap rather
        // than a defect in what this test covers.
        std::printf("  skip: engine not created (status %d) — is %s present?\n",
                    static_cast<int>(status), llamaModel.c_str());
        return;
    }

    assert(engine->warmUp() == EngineStatus::Ok && "weights should load");

    // Push in realistic slices: the ring buffer is the realtime path, and a
    // single giant write would not exercise it the way the audio tap does.
    constexpr std::size_t kChunk = 1600;        // 100 ms
    for (std::size_t offset = 0; offset < pcm.size(); offset += kChunk) {
        const std::size_t n = std::min(kChunk, pcm.size() - offset);
        engine->pushAudio(pcm.data() + offset, n);
        engine->runTranscriptionPass();
    }

    // The window only decodes once it is full; a short fixture finishes with
    // audio still buffered, which is exactly what the flush is for.
    engine->flush();

    const auto segments = engine->drainSegments();
    assert(!segments.empty() && "speech should produce at least one segment");

    std::string all;
    for (const auto& segment : segments) {
        all += " " + segment.text;
        assert(segment.endMs >= segment.startMs && "segment should not end before it starts");
    }
    all = lowercased(all);
    std::printf("  transcript:%s\n", all.c_str());

    // Substrings, not an exact match: a decoder is allowed to differ on
    // punctuation and casing without the engine being wrong.
    assert(all.find("quick") != std::string::npos);
    assert(all.find("brown fox") != std::string::npos);
    assert(all.find("lazy dog") != std::string::npos);

    // The global shim is the public way to drop a reference; release() itself
    // lives on the impl, which this test cannot see by design.
    scribaticEngineRelease(engine);
}

} // namespace

int main() {
    std::printf("TranscriptionTests\n");
    testSpeechBecomesTranscript();
    std::printf("ok\n");
    return 0;
}
