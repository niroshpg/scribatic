// =============================================================================
//  SpeakerTests.cpp — a real two-person conversation, end to end.
//
//  Transcribes a recording through the public interface exactly as the apps do
//  (session, ring buffer, flush, save), then identifies its speakers from the
//  saved note. Skips when the weights, the diarization models or the fixtures
//  are absent: `make fetch-models` provides all three.
// =============================================================================
#include "RecordingAudio.hpp"
#include "scribatic/core/EngineInterface.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace scribatic::core;

namespace {

std::string repoPath(const char* relative) {
    const char* root = std::getenv("SCRIBATIC_ROOT");
    return std::string(root != nullptr ? root : "../..") + "/" + relative;
}

bool exists(const std::string& path) {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0;
}

std::string squashed(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c != ' ') { out.push_back(c); }
    }
    return out;
}

void copyFile(const std::string& from, const std::string& to) {
    std::ifstream in(from, std::ios::binary);
    std::ofstream out(to, std::ios::binary);
    out << in.rdbuf();
}

struct Models {
    std::string whisper = repoPath("models/ggml-base.bin");
    std::string llama = repoPath("models/insight-q4_k_m.gguf");
    std::string segmentation = repoPath("models/speaker-segmentation.onnx");
    std::string embedding = repoPath("models/speaker-embedding.onnx");
    std::string accurate = repoPath("models/ggml-small-q8_0.bin");

    bool present() const {
        for (const auto* path : {&whisper, &llama, &segmentation, &embedding}) {
            if (!exists(*path)) {
                std::printf("  skip: %s missing (run `make fetch-models`)\n", path->c_str());
                return false;
            }
        }
        return true;
    }
};

void testConversationIsSplitBetweenTwoSpeakers(const Models& models, const char* fixtureName) {
    const std::string fixture = repoPath(fixtureName);
    if (!exists(fixture)) {
        std::printf("  skip: no fixture at %s\n", fixture.c_str());
        return;
    }
    std::printf("  %s\n", fixtureName);

    char pattern[] = "/tmp/scribatic-speakers-XXXXXX";
    const std::string dir = ::mkdtemp(pattern);
    const std::string recordings = dir + "/recordings";
    assert(::mkdir(recordings.c_str(), 0700) == 0);
    const std::string recording = recordings + "/recording-1.wav";

    EngineConfig config;
    config.whisperModelPath = models.whisper;
    config.llamaModelPath = models.llama;
    config.segmentationModelPath = models.segmentation;
    config.speakerEmbeddingModelPath = models.embedding;
    config.databasePath = dir + "/notes.sqlite";
    config.recordingsDirectory = recordings;
    config.threadCount = 4;

    EngineStatus status = EngineStatus::Ok;
    EngineInterface* engine = EngineInterface::create(config, &status);
    assert(engine != nullptr);
    assert(engine->warmUp() == EngineStatus::Ok);
    if (!engine->canIdentifySpeakers()) {
        std::printf("  skip: built without sherpa-onnx (run `make fetch-deps`)\n");
        scribaticEngineRelease(engine);
        return;
    }

    // After warmUp, as in the apps: warmUp sweeps recordings no note refers
    // to, and a file placed before it would be swept as an orphan.
    copyFile(fixture, recording);
    RecordingAudio audio;
    assert(audio.open(recording));

    engine->beginSession();
    constexpr std::size_t kChunk = 1600;                    // 100 ms, as the tap delivers
    for (std::size_t offset = 0; offset < audio.count(); offset += kChunk) {
        engine->pushAudio(audio.samples() + offset, std::min(kChunk, audio.count() - offset));
        engine->runTranscriptionPass();
    }
    engine->flush();
    (void)engine->drainSegments();

    const std::int64_t id = engine->saveSession("Two speakers", 1700000000, recording);
    assert(id > 0);
    const NoteDetail before = engine->loadNote(id);
    assert(!before.segments.empty());
    assert(before.speakerCount == 0 && "no speakers until identified");
    assert(before.durationMs > 10000);

    const auto started = std::chrono::steady_clock::now();
    assert(engine->identifySpeakers(id, 0) == EngineStatus::Ok);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    const NoteDetail after = engine->loadNote(id);
    std::printf("    %lld ms audio, diarized in %lld ms: %d speakers, %zu segments\n",
                static_cast<long long>(after.durationMs), static_cast<long long>(elapsed.count()),
                after.speakerCount, after.segments.size());

    // Estimating is allowed to over-split (ADR-009: 0.7 keeps voices apart
    // at the cost of sometimes splitting one), never to merge two people.
    assert(after.speakerCount >= 2);
    assert(after.speakers.size() == static_cast<std::size_t>(after.speakerCount));

    int changes = 0;
    std::string textBefore, textAfter;
    for (const auto& s : before.segments) { textBefore += s.text; }
    for (std::size_t i = 0; i < after.segments.size(); ++i) {
        const auto& s = after.segments[i];
        std::printf("    [%5.1fs] Speaker %d: %s\n", s.startMs / 1000.0, s.speaker + 1,
                    s.text.c_str());
        assert(s.speaker >= 0 && s.speaker < after.speakerCount);
        assert(s.endMs >= s.startMs);
        if (i > 0 && s.speaker != after.segments[i - 1].speaker) { ++changes; }
        textAfter += s.text;
    }
    assert(after.segments.front().speaker == 0 && "whoever speaks first is Speaker 1");
    assert(changes >= 1 && "a conversation changes hands");
    assert(squashed(textBefore) == squashed(textAfter) &&
           "re-cutting by speaker must not add, drop or reorder a single word");

    // A known count must be honoured, and re-running replaces rather than adds.
    assert(engine->renameSpeaker(id, 0, "Alex") == EngineStatus::Ok);
    assert(engine->identifySpeakers(id, 2) == EngineStatus::Ok);
    const NoteDetail again = engine->loadNote(id);
    std::printf("    told there are 2: %d speakers, %zu segments\n", again.speakerCount,
                again.segments.size());
    for (const auto& s : again.segments) {
        std::printf("    [%5.1fs] Speaker %d: %s\n", s.startMs / 1000.0, s.speaker + 1,
                    s.text.c_str());
    }
    assert(again.speakerCount == 2);
    assert(again.speakers[0].displayName == "Speaker 1" && "re-identifying resets names");

    // With the audio gone, identification is no longer possible, but the
    // attributed transcript stays exactly as it was.
    assert(engine->deleteRecording(id) == EngineStatus::Ok);
    assert(engine->identifySpeakers(id, 0) == EngineStatus::RecordingNotFound);
    assert(engine->loadNote(id).segments.size() == again.segments.size());

    scribaticEngineRelease(engine);
}

} // namespace

/// Four people, one of whom says little: estimating must keep all four. A
/// rule that folded speakers with under 2.5 s of speech into others fixed a
/// two-person chat that came back as six, and merged this quiet speaker away;
/// merged people cannot be told apart again afterwards.
void testFourVoicesStayApart(const Models& models) {
    const std::string fixture = repoPath("models/fixtures/four-speakers-en.wav");
    if (!exists(fixture)) {
        std::printf("  skip: no fixture at %s\n", fixture.c_str());
        return;
    }
    std::printf("  four-speakers-en.wav\n");
    char pattern[] = "/tmp/scribatic-four-XXXXXX";
    const std::string dir = ::mkdtemp(pattern);
    const std::string recordings = dir + "/recordings";
    assert(::mkdir(recordings.c_str(), 0700) == 0);
    const std::string recording = recordings + "/recording-1.wav";
    EngineConfig config;
    config.whisperModelPath = models.whisper;
    config.llamaModelPath = models.llama;
    config.segmentationModelPath = models.segmentation;
    config.speakerEmbeddingModelPath = models.embedding;
    config.databasePath = dir + "/notes.sqlite";
    config.recordingsDirectory = recordings;
    config.threadCount = 4;
    EngineStatus status = EngineStatus::Ok;
    EngineInterface* engine = EngineInterface::create(config, &status);
    assert(engine != nullptr && engine->warmUp() == EngineStatus::Ok);
    if (!engine->canIdentifySpeakers()) { scribaticEngineRelease(engine); return; }
    copyFile(fixture, recording);
    RecordingAudio audio;
    assert(audio.open(recording));
    engine->beginSession();
    for (std::size_t offset = 0; offset < audio.count(); offset += 1600) {
        engine->pushAudio(audio.samples() + offset, std::min<std::size_t>(1600, audio.count() - offset));
        engine->runTranscriptionPass();
    }
    engine->flush();
    (void)engine->drainSegments();
    const std::int64_t id = engine->saveSession("Four", 1700000000, recording);
    assert(engine->identifySpeakers(id, 0) == EngineStatus::Ok);
    const std::int32_t count = engine->loadNote(id).speakerCount;
    std::printf("    estimated %d speakers\n", count);
    assert(count >= 4 && "four voices must not be merged into fewer");
    scribaticEngineRelease(engine);
}

/// After Stop: the whole recording again with the accurate model, then
/// speakers from its words. The note ends up refined, with speakers.
void testRefineThenIdentify(const Models& models) {
    const std::string fixture = repoPath("models/fixtures/two-speakers-en-2.wav");
    if (!exists(fixture) || !exists(models.accurate)) {
        std::printf("  skip: refine needs the fixture and %s\n", models.accurate.c_str());
        return;
    }
    std::printf("  refine two-speakers-en-2.wav\n");
    char pattern[] = "/tmp/scribatic-refine-XXXXXX";
    const std::string dir = ::mkdtemp(pattern);
    const std::string recordings = dir + "/recordings";
    assert(::mkdir(recordings.c_str(), 0700) == 0);
    const std::string recording = recordings + "/recording-1.wav";
    EngineConfig config;
    config.whisperModelPath = models.whisper;
    config.llamaModelPath = models.llama;
    config.segmentationModelPath = models.segmentation;
    config.speakerEmbeddingModelPath = models.embedding;
    config.accurateModelPath = models.accurate;
    config.databasePath = dir + "/notes.sqlite";
    config.recordingsDirectory = recordings;
    config.threadCount = 4;
    EngineStatus status = EngineStatus::Ok;
    EngineInterface* engine = EngineInterface::create(config, &status);
    assert(engine != nullptr && engine->warmUp() == EngineStatus::Ok);
    assert(engine->canRefine());
    copyFile(fixture, recording);
    RecordingAudio audio;
    assert(audio.open(recording));
    engine->beginSession();
    for (std::size_t offset = 0; offset < audio.count(); offset += 1600) {
        engine->pushAudio(audio.samples() + offset, std::min<std::size_t>(1600, audio.count() - offset));
        engine->runTranscriptionPass();
    }
    engine->flush();
    (void)engine->drainSegments();
    const std::int64_t id = engine->saveSession("Refine", 1700000000, recording);
    assert(!engine->loadNote(id).refined);

    assert(engine->refineTranscript(id) == EngineStatus::Ok);
    assert(engine->refineProgress() == 1.0F);
    NoteDetail note = engine->loadNote(id);
    assert(note.refined && !note.segments.empty());
    for (const auto& s : note.segments) { std::printf("    refined: %s\n", s.text.c_str()); }

    if (engine->canIdentifySpeakers()) {
        assert(engine->identifySpeakers(id, 0) == EngineStatus::Ok);
        note = engine->loadNote(id);
        std::printf("    %d speakers after refining\n", note.speakerCount);
        assert(note.speakerCount >= 2 && note.refined);
    }
    scribaticEngineRelease(engine);
}

/// Prints what speaker identification makes of any recording: estimated and
/// told-two. For investigating a tester's note; asserts nothing.
void diagnoseRecording(const Models& models, const std::string& wav) {
    std::printf("  diagnose %s\n", wav.c_str());
    char pattern[] = "/tmp/scribatic-diagnose-XXXXXX";
    const std::string dir = ::mkdtemp(pattern);
    const std::string recordings = dir + "/recordings";
    ::mkdir(recordings.c_str(), 0700);
    const std::string recording = recordings + "/recording-1.wav";
    EngineConfig config;
    config.whisperModelPath = models.whisper;
    config.llamaModelPath = models.llama;
    config.segmentationModelPath = models.segmentation;
    config.speakerEmbeddingModelPath = models.embedding;
    config.accurateModelPath = models.accurate;
    config.databasePath = dir + "/notes.sqlite";
    config.recordingsDirectory = recordings;
    config.threadCount = 4;
    EngineStatus status = EngineStatus::Ok;
    EngineInterface* engine = EngineInterface::create(config, &status);
    if (engine == nullptr || engine->warmUp() != EngineStatus::Ok) { std::printf("    no engine\n"); return; }
    copyFile(wav, recording);
    RecordingAudio audio;
    if (!audio.open(recording)) { std::printf("    cannot read %s\n", wav.c_str()); scribaticEngineRelease(engine); return; }
    engine->beginSession();
    for (std::size_t offset = 0; offset < audio.count(); offset += 1600) {
        engine->pushAudio(audio.samples() + offset, std::min<std::size_t>(1600, audio.count() - offset));
        engine->runTranscriptionPass();
    }
    engine->flush();
    (void)engine->drainSegments();
    const std::int64_t id = engine->saveSession("Diagnose", 1700000000, recording);
    std::printf("    preview:");
    for (const auto& s : engine->loadNote(id).segments) { std::printf(" %s", s.text.c_str()); }
    std::printf("\n");
    if (engine->canRefine()) {
        const auto started = std::chrono::steady_clock::now();
        const EngineStatus refined = engine->refineTranscript(id);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        std::printf("    refined (%d) in %lld ms:", static_cast<int>(refined), static_cast<long long>(ms));
        for (const auto& s : engine->loadNote(id).segments) { std::printf(" %s", s.text.c_str()); }
        std::printf("\n");
    }
    for (const std::int32_t expected : {0, 2}) {
        engine->identifySpeakers(id, expected);
        const NoteDetail note = engine->loadNote(id);
        std::printf("    %s: %d speakers\n", expected == 0 ? "estimated" : "told 2", note.speakerCount);
        for (const auto& s : note.segments) {
            std::printf("      [%5.1fs] Speaker %d: %s\n", s.startMs / 1000.0, s.speaker + 1, s.text.c_str());
        }
    }
    scribaticEngineRelease(engine);
}

/// Two people, two languages: once speakers are known, each one's lines are
/// in their own language — not the recording's first, which whisper would
/// have translated the other speaker into, or garbled.
void testEachSpeakerInTheirLanguage(const Models& models) {
    const std::string fixture = repoPath("models/fixtures/two-speakers-en-es.wav");
    if (!exists(fixture)) {
        std::printf("  skip: no fixture at %s\n", fixture.c_str());
        return;
    }
    std::printf("  two-speakers-en-es.wav\n");
    char pattern[] = "/tmp/scribatic-bilingual-XXXXXX";
    const std::string dir = ::mkdtemp(pattern);
    const std::string recordings = dir + "/recordings";
    assert(::mkdir(recordings.c_str(), 0700) == 0);
    const std::string recording = recordings + "/recording-1.wav";

    EngineConfig config;
    config.whisperModelPath = models.whisper;
    config.llamaModelPath = models.llama;
    config.segmentationModelPath = models.segmentation;
    config.speakerEmbeddingModelPath = models.embedding;
    config.databasePath = dir + "/notes.sqlite";
    config.recordingsDirectory = recordings;
    config.threadCount = 4;
    EngineStatus status = EngineStatus::Ok;
    EngineInterface* engine = EngineInterface::create(config, &status);
    assert(engine != nullptr && engine->warmUp() == EngineStatus::Ok);
    if (!engine->canIdentifySpeakers()) {
        std::printf("  skip: built without sherpa-onnx\n");
        scribaticEngineRelease(engine);
        return;
    }
    copyFile(fixture, recording);
    RecordingAudio audio;
    assert(audio.open(recording));

    engine->setLanguage("auto");
    engine->beginSession();
    constexpr std::size_t kChunk = 1600;
    for (std::size_t offset = 0; offset < audio.count(); offset += kChunk) {
        engine->pushAudio(audio.samples() + offset, std::min(kChunk, audio.count() - offset));
        engine->runTranscriptionPass();
    }
    engine->flush();
    (void)engine->drainSegments();
    const std::int64_t id = engine->saveSession("Bilingual", 1700000000, recording);
    assert(id > 0);
    std::printf("    recording language: %s\n", engine->loadNote(id).language.c_str());

    assert(engine->identifySpeakers(id, 2) == EngineStatus::Ok);
    const NoteDetail after = engine->loadNote(id);
    std::string spanish, english;
    for (const auto& s : after.segments) {
        std::printf("    Speaker %d: %s\n", s.speaker + 1, s.text.c_str());
        (s.speaker == after.segments.front().speaker ? english : spanish) += " " + s.text;
    }
    auto has = [](const std::string& text, const char* word) { return text.find(word) != std::string::npos; };
    assert(after.speakerCount == 2);
    assert(has(english, "budget") && "the English speaker stays in English");
    assert((has(spanish, "presupuesto") || has(spanish, "marzo")) && "the Spanish speaker is transcribed in Spanish");
    scribaticEngineRelease(engine);
}

int main() {
    std::printf("SpeakerTests\n");
    const Models models;
    if (models.present()) {
        testConversationIsSplitBetweenTwoSpeakers(models, "models/fixtures/two-speakers-en-2.wav");
        testConversationIsSplitBetweenTwoSpeakers(models, "models/fixtures/two-speakers-en-3.wav");
        testEachSpeakerInTheirLanguage(models);
        testFourVoicesStayApart(models);
        testRefineThenIdentify(models);
        // A recording to look at, not to assert on: SCRIBATIC_SPEAKERS_WAV=/path.wav
        if (const char* extra = std::getenv("SCRIBATIC_SPEAKERS_WAV"); extra != nullptr) {
            diagnoseRecording(models, extra);
        }
    }
    std::printf("ok\n");
    return 0;
}
