// =============================================================================
//  Replay.cpp — runs a recording through the pipeline the apps use, stage by
//  stage, to see where words go missing.
//
//      scribatic_replay <recording.wav> [--speakers N] [--reference model.bin]
//      scribatic_replay <recording.wav> --windows [reference.txt]
//
//  The live path (5 s windows, as the recorder streams it), the accurate pass
//  after Stop, and speaker identification, each printed with times. Speech in
//  the audio that no segment covers is listed: words that never reached
//  recognition, or that it dropped. With --reference, a larger whisper model
//  transcribes the same audio and the final transcript is scored against it.
//
//  Models come from $SCRIBATIC_ROOT/models (default: the working directory's
//  ../..), as the tests find them. Host-only; nothing here ships.
// =============================================================================
#include "RecordingAudio.hpp"
#include "SpeakerDiarizer.hpp"
#include "scribatic/core/EngineInterface.hpp"
#include "whisper.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace scribatic::core;

namespace {

constexpr std::size_t kRate = 16000;

std::string root() {
    const char* env = std::getenv("SCRIBATIC_ROOT");
    return env != nullptr ? env : "../..";
}

std::string clock(std::int64_t ms) {
    char out[16];
    std::snprintf(out, sizeof out, "%3lld.%01lld", static_cast<long long>(ms / 1000),
                  static_cast<long long>((ms % 1000) / 100));
    return out;
}

double dbfs(const float* samples, std::size_t count) {
    if (count == 0) { return -120.0; }
    double energy = 0.0;
    for (std::size_t i = 0; i < count; ++i) { energy += static_cast<double>(samples[i]) * samples[i]; }
    return 10.0 * std::log10(energy / static_cast<double>(count) + 1e-12);
}

void printSegments(const char* title, const std::vector<TranscriptSegment>& segments) {
    std::printf("\n== %s: %zu segments\n", title, segments.size());
    for (const auto& s : segments) {
        std::printf("  %s-%s %s%s\n", clock(s.startMs).c_str(), clock(s.endMs).c_str(),
                    s.speaker >= 0 ? ("S" + std::to_string(s.speaker + 1) + " ").c_str() : "",
                    s.text.c_str());
    }
}

/// Stretches of speech-level audio (100 ms frames above `floorDb`, bridged
/// over gaps under 300 ms) that no segment overlaps. A word whisper heard but
/// dropped, or one that never reached it, shows up here.
void printUncovered(const char* title, const RecordingAudio& audio,
                    const std::vector<TranscriptSegment>& segments, double floorDb) {
    constexpr std::size_t kFrame = kRate / 10;
    const std::size_t frames = audio.count() / kFrame;
    std::vector<bool> covered(frames, false);
    for (const auto& s : segments) {
        for (std::int64_t f = s.startMs / 100; f <= s.endMs / 100 && f < static_cast<std::int64_t>(frames); ++f) {
            if (f >= 0) { covered[static_cast<std::size_t>(f)] = true; }
        }
    }
    std::printf("\n== %s: speech above %.0f dBFS with no segment\n", title, floorDb);
    int found = 0;
    std::size_t f = 0;
    while (f < frames) {
        const auto loud = [&](std::size_t k) { return dbfs(audio.samples() + k * kFrame, kFrame) > floorDb; };
        if (!loud(f) || covered[f]) { ++f; continue; }
        std::size_t end = f, quiet = 0;
        for (std::size_t k = f; k < frames && !covered[k]; ++k) {
            if (loud(k)) { end = k; quiet = 0; } else if (++quiet > 3) { break; }
        }
        const std::size_t length = end - f + 1;
        if (length >= 4) {   // 400 ms: a word, not a click
            std::printf("  %s-%s  %.1f s at %.1f dBFS\n", clock(static_cast<std::int64_t>(f) * 100).c_str(),
                        clock(static_cast<std::int64_t>(end + 1) * 100).c_str(), length / 10.0,
                        dbfs(audio.samples() + f * kFrame, length * kFrame));
            ++found;
        }
        f = end + 1;
    }
    if (found == 0) { std::printf("  none\n"); }
}

std::vector<std::string> normalisedWords(const std::string& text) {
    std::vector<std::string> out;
    std::string word;
    for (const char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '\'') {
            word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (!word.empty()) {
            while (!word.empty() && word.back() == '.') { word.pop_back(); }
            if (!word.empty()) { out.push_back(word); }
            word.clear();
        }
    }
    while (!word.empty() && word.back() == '.') { word.pop_back(); }
    if (!word.empty()) { out.push_back(word); }
    return out;
}

/// Word error rate of `hypothesis` against `reference`, with the edits.
void score(const std::string& title, const std::string& reference, const std::string& hypothesis) {
    const auto ref = normalisedWords(reference);
    const auto hyp = normalisedWords(hypothesis);
    const std::size_t n = ref.size(), m = hyp.size();
    std::vector<std::vector<std::size_t>> d(n + 1, std::vector<std::size_t>(m + 1, 0));
    for (std::size_t i = 0; i <= n; ++i) { d[i][0] = i; }
    for (std::size_t j = 0; j <= m; ++j) { d[0][j] = j; }
    for (std::size_t i = 1; i <= n; ++i) {
        for (std::size_t j = 1; j <= m; ++j) {
            d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1,
                                d[i - 1][j - 1] + (ref[i - 1] == hyp[j - 1] ? 0 : 1)});
        }
    }
    std::vector<std::string> edits;
    std::size_t i = n, j = m, deletions = 0, insertions = 0, substitutions = 0;
    while (i > 0 || j > 0) {
        if (i > 0 && j > 0 && d[i][j] == d[i - 1][j - 1] + (ref[i - 1] == hyp[j - 1] ? 0 : 1)) {
            if (ref[i - 1] != hyp[j - 1]) { edits.push_back(ref[i - 1] + " -> " + hyp[j - 1]); ++substitutions; }
            --i; --j;
        } else if (i > 0 && d[i][j] == d[i - 1][j] + 1) {
            edits.push_back("missing: " + ref[i - 1]); ++deletions; --i;
        } else {
            edits.push_back("extra: " + hyp[j - 1]); ++insertions; --j;
        }
    }
    std::reverse(edits.begin(), edits.end());
    std::printf("\n== %s vs reference: WER %.1f%% (%zu words; %zu substituted, %zu missing, %zu extra)\n",
                title.c_str(), n == 0 ? 0.0 : 100.0 * d[n][m] / static_cast<double>(n), n,
                substitutions, deletions, insertions);
    for (const auto& edit : edits) { std::printf("  %s\n", edit.c_str()); }
}

std::string joined(const std::vector<TranscriptSegment>& segments) {
    std::string out;
    for (const auto& s : segments) { out += " " + s.text; }
    return out;
}

std::string reference(const std::string& modelPath, const RecordingAudio& audio) {
    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = false;
    whisper_context* ctx = whisper_init_from_file_with_params(modelPath.c_str(), cparams);
    if (ctx == nullptr) { return {}; }
    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
    params.n_threads = 8;
    params.language = "auto";
    params.print_progress = false;
    params.print_realtime = false;
    params.print_timestamps = false;
    std::string text;
    if (whisper_full(ctx, params, audio.samples(), static_cast<int>(audio.count())) == 0) {
        std::printf("\n== reference (%s): language %s\n", modelPath.c_str(),
                    whisper_lang_str(whisper_full_lang_id(ctx)));
        for (int k = 0; k < whisper_full_n_segments(ctx); ++k) {
            const char* piece = whisper_full_get_segment_text(ctx, k);
            std::printf("  %s-%s %s\n", clock(whisper_full_get_segment_t0(ctx, k) * 10).c_str(),
                        clock(whisper_full_get_segment_t1(ctx, k) * 10).c_str(), piece);
            text += piece;
        }
    }
    whisper_free(ctx);
    return text;
}

/// Copies `count` samples scaled so their RMS is `targetDb`, the gain capped
/// at `maxGainDb` so near-silence is not raised into noise.
std::vector<float> normalised(const float* samples, std::size_t count, double targetDb, double maxGainDb) {
    const double gainDb = std::min(maxGainDb, std::max(0.0, targetDb - dbfs(samples, count)));
    const float gain = static_cast<float>(std::pow(10.0, gainDb / 20.0));
    std::vector<float> out(samples, samples + count);
    for (float& sample : out) { sample = std::clamp(sample * gain, -1.0F, 1.0F); }
    return out;
}

/// The live windows decoded under variants of the live settings, to see which
/// one loses or invents words.
void windows(const std::string& modelPath, const RecordingAudio& audio, const std::string& referenceText) {
    struct Variant { const char* name; std::size_t seconds; bool gain; bool prompt; };
    const Variant variants[] = {
        {"live: 5 s", 5, false, false},
        {"5 s, gain to -25 dBFS", 5, true, false},
        {"5 s, gain, previous text as prompt", 5, true, true},
        {"10 s, gain, prompt", 10, true, true},
    };
    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = false;
    whisper_context* ctx = whisper_init_from_file_with_params(modelPath.c_str(), cparams);
    if (ctx == nullptr) { return; }
    for (const auto& v : variants) {
        std::printf("\n== windows: %s\n", v.name);
        std::string all, previous;
        for (std::size_t start = 0; start + kRate / 2 < audio.count(); start += v.seconds * kRate) {
            const std::size_t n = std::min<std::size_t>(v.seconds * kRate, audio.count() - start);
            const std::vector<float> window = v.gain ? normalised(audio.samples() + start, n, -25.0, 30.0)
                                                     : std::vector<float>(audio.samples() + start, audio.samples() + start + n);
            whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
            params.n_threads = 8;
            params.language = "en";
            params.print_progress = false;
            params.print_realtime = false;
            params.print_timestamps = false;
            if (v.prompt && !previous.empty()) { params.initial_prompt = previous.c_str(); }
            std::string text;
            if (whisper_full(ctx, params, window.data(), static_cast<int>(window.size())) == 0) {
                for (int k = 0; k < whisper_full_n_segments(ctx); ++k) {
                    const std::string piece = whisper_full_get_segment_text(ctx, k);
                    const auto first = piece.find_first_not_of(' ');
                    if (first != std::string::npos && (piece[first] == '[' || piece[first] == '(')) { continue; }
                    text += piece;
                }
            }
            std::printf("  %s %s\n", clock(static_cast<std::int64_t>(start * 1000 / kRate)).c_str(), text.c_str());
            all += text;
            previous = text;
        }
        if (!referenceText.empty()) {
            const auto ref = normalisedWords(referenceText);
            const auto hyp = normalisedWords(all);
            std::vector<std::vector<std::size_t>> d(ref.size() + 1, std::vector<std::size_t>(hyp.size() + 1, 0));
            for (std::size_t i = 0; i <= ref.size(); ++i) { d[i][0] = i; }
            for (std::size_t j = 0; j <= hyp.size(); ++j) { d[0][j] = j; }
            for (std::size_t i = 1; i <= ref.size(); ++i) {
                for (std::size_t j = 1; j <= hyp.size(); ++j) {
                    d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + (ref[i - 1] == hyp[j - 1] ? 0 : 1)});
                }
            }
            std::printf("  WER %.1f%%\n", 100.0 * d[ref.size()][hyp.size()] / static_cast<double>(ref.size()));
        }
    }
    whisper_free(ctx);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s recording.wav [--windows | --speakers N --reference model.bin --floor dBFS]\n", argv[0]);
        return 2;
    }
    const std::string input = argv[1];
    std::int32_t speakers = 0;
    std::string referenceModel;
    double floorDb = -45.0;
    for (int k = 2; k + 1 < argc; k += 2) {
        const std::string flag = argv[k];
        if (flag == "--speakers") { speakers = std::atoi(argv[k + 1]); }
        else if (flag == "--reference") { referenceModel = argv[k + 1]; }
        else if (flag == "--floor") { floorDb = std::atof(argv[k + 1]); }
    }
    const bool windowsOnly = argc >= 3 && std::string(argv[2]) == "--windows";

    RecordingAudio audio;
    if (!audio.open(input)) { std::fprintf(stderr, "not a 16 kHz mono WAV: %s\n", input.c_str()); return 1; }
    std::printf("%s: %.1f s, %.1f dBFS overall\n", input.c_str(), audio.count() / double(kRate),
                dbfs(audio.samples(), audio.count()));
    if (windowsOnly) {
        std::string ref;
        if (argc >= 4) { std::ifstream file(argv[3]); std::stringstream all; all << file.rdbuf(); ref = all.str(); }
        windows(root() + "/models/ggml-base.bin", audio, ref);
        return 0;
    }

    char pattern[] = "/tmp/scribatic-replay-XXXXXX";
    const std::string dir = ::mkdtemp(pattern);
    const std::string recordings = dir + "/recordings";
    ::mkdir(recordings.c_str(), 0700);
    const std::string copy = recordings + "/recording-1.wav";
    { std::ifstream from(input, std::ios::binary); std::ofstream to(copy, std::ios::binary); to << from.rdbuf(); }

    const std::string models = root() + "/models/";
    EngineConfig config;
    config.whisperModelPath = models + "ggml-base.bin";
    config.llamaModelPath = models + "insight-q4_k_m.gguf";
    config.accurateModelPath = models + "ggml-small-q8_0.bin";
    config.segmentationModelPath = models + "speaker-segmentation.onnx";
    config.speakerEmbeddingModelPath = models + "speaker-embedding.onnx";
    config.databasePath = dir + "/notes.sqlite";
    config.recordingsDirectory = recordings;
    config.threadCount = 8;
    EngineStatus status = EngineStatus::Ok;
    EngineInterface* engine = EngineInterface::create(config, &status);
    if (engine == nullptr || engine->warmUp() != EngineStatus::Ok) {
        std::fprintf(stderr, "engine: %s\n", describeStatus(status).c_str());
        return 1;
    }

    // Live: 100 ms pushes with a pass after each, as the recorder does.
    engine->beginSession();
    constexpr std::size_t kChunk = kRate / 10;
    for (std::size_t offset = 0; offset < audio.count(); offset += kChunk) {
        engine->pushAudio(audio.samples() + offset, std::min(kChunk, audio.count() - offset));
        engine->runTranscriptionPass();
    }
    engine->flush();
    const auto live = engine->drainSegments();
    std::printf("\nlive language: %s\n", engine->sessionLanguage().c_str());
    printSegments("live (5 s windows)", live);
    printUncovered("live", audio, live, floorDb);

    const std::int64_t id = engine->saveSession("Replay", 0, copy);
    const EngineStatus refined = engine->refineTranscript(id, "");
    const NoteDetail accurate = engine->loadNote(id);
    std::printf("\naccurate pass: %s, language %s\n", describeStatus(refined).c_str(), accurate.language.c_str());
    printSegments("accurate", accurate.segments);
    printUncovered("accurate", audio, accurate.segments, floorDb);

    // The diarizer's own turns, before words are attributed to them.
    SpeakerDiarizer diarizer;
    EngineStatus turnsStatus = EngineStatus::Ok;
    const auto turns = diarizer.run(config.segmentationModelPath, config.speakerEmbeddingModelPath, 8,
                                    audio.samples(), audio.count(), speakers, &turnsStatus);
    std::printf("\n== diarizer turns (%s): %zu\n", describeStatus(turnsStatus).c_str(), turns.size());
    std::map<std::int32_t, std::int64_t> talk;
    for (std::size_t k = 0; k < turns.size(); ++k) {
        const auto& t = turns[k];
        const std::int64_t gap = k == 0 ? 0 : t.startMs - turns[k - 1].endMs;
        std::printf("  %s-%s cluster %d%s\n", clock(t.startMs).c_str(), clock(t.endMs).c_str(), t.speaker,
                    k > 0 && t.speaker != turns[k - 1].speaker
                        ? ("  <- change after " + std::to_string(gap) + " ms gap").c_str() : "");
        talk[t.speaker] += t.endMs - t.startMs;
    }
    for (const auto& [cluster, ms] : talk) { std::printf("  cluster %d: %.1f s\n", cluster, ms / 1000.0); }

    const EngineStatus identified = engine->identifySpeakers(id, speakers, "");
    const NoteDetail final = engine->loadNote(id);
    std::printf("\nspeakers: %s, %d identified\n", describeStatus(identified).c_str(), final.speakerCount);
    printSegments("final", final.segments);

    if (!referenceModel.empty()) {
        const std::string ref = reference(referenceModel, audio);
        if (!ref.empty()) {
            score("live", ref, joined(live));
            score("final", ref, joined(final.segments));
        }
    }
    scribaticEngineRelease(engine);
    return 0;
}
