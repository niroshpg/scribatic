// =============================================================================
//  NoteStoreTests.cpp — persistence, deletion and speaker attribution.
//
//  Needs no weights and no diarization models, so it runs everywhere CI does.
//  The deletion tests look at the database with a second connection rather
//  than through the store's own API: the claim under test is that nothing is
//  left behind, and the store is the last thing to ask about that.
// =============================================================================
#include "NoteStore.hpp"
#include "SpeakerAttribution.hpp"
#include "TranscriptFormat.hpp"
#include "scribatic/core/EngineInterface.hpp"
#include "scribatic/db/Schema.hpp"
#include "sqlite3.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace scribatic::core;

namespace {

std::string tempDirectory() {
    char pattern[] = "/tmp/scribatic-store-XXXXXX";
    const char* made = ::mkdtemp(pattern);
    assert(made != nullptr);
    return made;
}

bool exists(const std::string& path) {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0;
}

void touch(const std::string& path, const std::string& contents = "x") {
    std::ofstream(path, std::ios::binary) << contents;
}

std::int64_t scalar(const std::string& db, const std::string& sql) {
    sqlite3* handle = nullptr;
    assert(sqlite3_open_v2(db.c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    assert(sqlite3_prepare_v2(handle, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
    std::int64_t value = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) { value = sqlite3_column_int64(stmt, 0); }
    sqlite3_finalize(stmt);
    sqlite3_close(handle);
    return value;
}

/// Number of distinct terms in the FTS index — the index itself, not the
/// content table it is joined to, which is what a stale index hides behind.
std::int64_t indexedTerms(const std::string& db) {
    sqlite3* handle = nullptr;
    assert(sqlite3_open_v2(db.c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
    assert(sqlite3_exec(handle,
                        "CREATE VIRTUAL TABLE IF NOT EXISTS temp.vocab "
                        "USING fts5vocab(main, chunks_fts, 'row');",
                        nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    assert(sqlite3_prepare_v2(handle, "SELECT count(*) FROM temp.vocab;", -1, &stmt, nullptr) ==
           SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    const std::int64_t terms = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    // With external content, a trigger-less delete leaves the index pointing at
    // rows that no longer exist; integrity-check is what notices.
    const int check = sqlite3_exec(
        handle, "INSERT INTO chunks_fts(chunks_fts) VALUES('integrity-check');", nullptr,
        nullptr, nullptr);
    assert(check == SQLITE_OK && "FTS index must match its content table");
    sqlite3_close(handle);
    return terms;
}

WordTiming word(std::int32_t segment, std::int64_t start, std::int64_t end, const char* text) {
    WordTiming w;
    w.segment = segment;
    w.startMs = start;
    w.endMs = end;
    w.text = text;
    w.probability = 0.9F;
    return w;
}

TranscriptSegment segment(std::int64_t start, std::int64_t end, const char* text) {
    TranscriptSegment s;
    s.startMs = start;
    s.endMs = end;
    s.text = text;
    s.isFinal = true;
    return s;
}

// -- Attribution ----------------------------------------------------------------

void testSegmentSpanningTwoSpeakersIsCut() {
    // One whisper segment, two voices: "Are you coming? Yes, I am."
    const std::vector<WordTiming> words{
        word(0, 0, 300, " Are"),   word(0, 300, 500, " you"), word(0, 500, 900, " coming?"),
        word(0, 1200, 1500, " Yes,"), word(0, 1500, 1700, " I"), word(0, 1700, 2000, " am."),
    };
    // Cluster ids deliberately sparse and out of order, as the diarizer emits.
    const std::vector<SpeakerTurn> turns{{0, 1000, 4}, {1100, 2100, 1}};

    const Attribution result = attributeSpeakers(words, turns);
    assert(result.speakerCount == 2);
    assert(result.segments.size() == 2);
    assert(result.segments[0].text == "Are you coming?");
    assert(result.segments[0].speaker == 0 && "first voice heard is Speaker 1");
    assert(result.segments[1].text == "Yes, I am.");
    assert(result.segments[1].speaker == 1);
    assert(result.segments[1].startMs == 1200 && result.segments[1].endMs == 2000);
}

void testWhisperBoundariesSurvive() {
    const std::vector<WordTiming> words{
        word(0, 0, 400, " First."), word(1, 500, 900, " Second."),
    };
    const Attribution result = attributeSpeakers(words, {{0, 1000, 7}});
    assert(result.segments.size() == 2 && "same speaker, but two whisper segments");
    assert(result.speakerCount == 1);
}

void testIslandWordIsSmoothed() {
    const std::vector<WordTiming> words{
        word(0, 0, 400, " one"), word(0, 400, 700, " two"), word(0, 700, 1100, " three"),
    };
    // The middle word overlaps a 250 ms blip of another cluster.
    const std::vector<SpeakerTurn> turns{{0, 420, 0}, {420, 670, 3}, {670, 1100, 0}};
    const Attribution result = attributeSpeakers(words, turns);
    assert(result.segments.size() == 1);
    assert(result.segments[0].text == "one two three");
    assert(result.speakerCount == 1);
}

void testWordInSilenceTakesNearestTurn() {
    const std::vector<WordTiming> words{
        word(0, 0, 400, " hello"), word(1, 5000, 5400, " again"),
    };
    const std::vector<SpeakerTurn> turns{{0, 450, 2}, {5600, 7000, 9}};
    const Attribution result = attributeSpeakers(words, turns);
    assert(result.segments.size() == 2);
    assert(result.segments[1].speaker == 1 && "a word whisper heard was said by somebody");
}

void testTurnFinalWordStaysWithItsSpeaker() {
    // "...what is it?" [1.2 s pause] "Well..." — the stored span of "it?" runs
    // from its onset to the next onset, straight across the pause.
    const std::vector<WordTiming> words{
        word(0, 0, 400, " What"), word(0, 400, 800, " is"), word(0, 800, 2400, " it?"),
        word(0, 2400, 2900, " Well,"), word(0, 2900, 3400, " it"),
    };
    const std::vector<SpeakerTurn> turns{{0, 900, 0}, {2300, 3500, 1}};
    const Attribution result = attributeSpeakers(words, turns);
    assert(result.segments.size() == 2);
    assert(result.segments[0].text == "What is it?");
    assert(result.segments[1].text == "Well, it");
}

void testNoTurnsLeavesSpeakersUnknown() {
    const Attribution result = attributeSpeakers({word(0, 0, 400, " alone")}, {});
    assert(result.speakerCount == 0);
    assert(result.segments.size() == 1 && result.segments[0].speaker == -1);
}

// -- Formatting -------------------------------------------------------------------

NoteDetail sampleNote() {
    NoteDetail note;
    note.id = 1;
    note.title = "Excursion planning";
    note.createdAt = 0;
    note.durationMs = 65000;
    note.speakerCount = 2;
    note.speakers = {{0, "Ms Perera", ""}, {1, "", ""}};
    resolveDisplayNames(note.speakers);
    auto a = segment(0, 2000, "Let's start.");
    a.speaker = 0;
    auto b = segment(2000, 4000, "With the bus.");
    b.speaker = 0;
    auto c = segment(61000, 64000, "It leaves at eight.");
    c.speaker = 1;
    note.segments = {a, b, c};
    return note;
}

void testExportMergesTurnsAndNamesSpeakers() {
    const std::string text = formatTranscript(sampleNote(), {true, false});
    assert(text.find("Excursion planning\n") == 0);
    assert(text.find("1 min 5 s") != std::string::npos);
    assert(text.find("2 speakers") != std::string::npos);
    assert(text.find("[00:00] Ms Perera: Let's start. With the bus.") != std::string::npos);
    assert(text.find("[01:01] Speaker 2: It leaves at eight.") != std::string::npos);
}

void testAnonymisedExportCarriesNoNames() {
    const std::string text = formatTranscript(sampleNote(), {false, true});
    assert(text.find("Perera") == std::string::npos);
    assert(text.find("\nSpeaker 1: Let's start.") != std::string::npos);
    assert(text.find("[00:00]") == std::string::npos);
}

// -- Store ------------------------------------------------------------------------

void testSchemaMigratesToCurrentVersion() {
    const std::string dir = tempDirectory();
    const std::string db = dir + "/notes.sqlite";
    NoteStore store;
    assert(store.open(db) == EngineStatus::Ok);
    store.close();
    assert(scalar(db, "PRAGMA user_version;") == scribatic::db::kSchemaVersion);
    // Reopening an up-to-date file must be a no-op, not a second migration.
    assert(store.open(db) == EngineStatus::Ok);
}

void testDeletingANoteLeavesNothingSearchable() {
    const std::string dir = tempDirectory();
    const std::string db = dir + "/notes.sqlite";
    NoteStore store;
    assert(store.open(db) == EngineStatus::Ok);

    const std::int64_t id = store.insertNote(
        "Planning", 1700000000, 5000, "recording-1.wav",
        {segment(0, 2000, "The museum excursion"), segment(2000, 4000, "needs permission slips")},
        {word(0, 0, 1000, " The"), word(0, 1000, 2000, " museum excursion"),
         word(1, 2000, 4000, " needs permission slips")});
    assert(id > 0);
    assert(scalar(db, "SELECT count(*) FROM chunks_fts WHERE chunks_fts MATCH 'museum';") == 1);
    assert(indexedTerms(db) > 0);

    std::string audio;
    assert(store.deleteNote(id, &audio) == EngineStatus::Ok);
    assert(audio == "recording-1.wav" && "caller needs the file name to remove the file");

    for (const char* table : {"notes", "segments", "words", "chunks", "speakers"}) {
        assert(scalar(db, std::string("SELECT count(*) FROM ") + table + ";") == 0);
    }
    assert(indexedTerms(db) == 0 && "FTS rows must go with the note");
}

void testSpeakerNamesAreIndexedAndRenamable() {
    const std::string dir = tempDirectory();
    const std::string db = dir + "/notes.sqlite";
    NoteStore store;
    assert(store.open(db) == EngineStatus::Ok);

    const std::int64_t id = store.insertNote("Chat", 1700000000, 3000, "", {segment(0, 1000, "hi")},
                                             {word(0, 0, 1000, " hi")});
    auto attributed = segment(0, 1000, "hi");
    attributed.speaker = 0;
    assert(store.replaceTranscript(id, {attributed}, 1) == EngineStatus::Ok);

    NoteDetail detail = store.load(id, dir);
    assert(detail.speakerCount == 1);
    assert(detail.speakers.size() == 1 && detail.speakers[0].displayName == "Speaker 1");
    assert(detail.audioPath.empty() && "a note saved without audio has none");

    assert(store.renameSpeaker(id, 0, "Priya") == EngineStatus::Ok);
    assert(store.load(id, dir).speakers[0].displayName == "Priya");
    assert(scalar(db, "SELECT count(*) FROM chunks_fts WHERE chunks_fts MATCH 'priya';") == 1);
    assert(indexedTerms(db) > 0);

    assert(store.renameSpeaker(id, 5, "Nobody") == EngineStatus::DatabaseFailed);
    assert(store.renameSpeaker(id, 0, "") == EngineStatus::Ok);
    assert(store.load(id, dir).speakers[0].displayName == "Speaker 1");
    assert(scalar(db, "SELECT count(*) FROM chunks_fts WHERE chunks_fts MATCH 'priya';") == 0);
}

// -- Engine: recordings on disk -----------------------------------------------------

/// Drives the engine's file lifecycle without any weights. create() only checks
/// that model files exist, and warmUp() opens the store before it loads them, so
/// placeholder weights give a live store and a sweep and then fail the model load.
void testRecordingLifecycle() {
    const std::string dir = tempDirectory();
    const std::string recordings = dir + "/recordings";
    const std::string db = dir + "/notes.sqlite";
    assert(::mkdir(recordings.c_str(), 0700) == 0);

    std::int64_t kept = 0;
    std::int64_t dangling = 0;
    {
        NoteStore store;
        assert(store.open(db) == EngineStatus::Ok);
        kept = store.insertNote("Kept", 1700000000, 2000, "recording-1.wav",
                                {segment(0, 1000, "keep the words")},
                                {word(0, 0, 1000, " keep the words")});
        dangling = store.insertNote("Dangling", 1700000100, 2000, "recording-9.wav",
                                    {segment(0, 1000, "file is gone")},
                                    {word(0, 0, 1000, " file is gone")});
    }
    touch(recordings + "/recording-1.wav");
    touch(recordings + "/recording-2.wav");           // orphan: no note refers to it
    touch(recordings + "/notes-to-self.txt");         // not ours: must survive the sweep
    touch(dir + "/whisper.bin", "not a model");
    touch(dir + "/llama.gguf", "not a model");

    EngineConfig config;
    config.whisperModelPath = dir + "/whisper.bin";
    config.llamaModelPath = dir + "/llama.gguf";
    config.databasePath = db;
    config.recordingsDirectory = recordings;
    EngineStatus status = EngineStatus::Ok;
    EngineInterface* engine = EngineInterface::create(config, &status);
    assert(engine != nullptr);
    assert(engine->warmUp() == EngineStatus::ModelLoadFailed);
    assert(!engine->canIdentifySpeakers());

    assert(exists(recordings + "/recording-1.wav"));
    assert(!exists(recordings + "/recording-2.wav") && "unreferenced recording is swept");
    assert(exists(recordings + "/notes-to-self.txt"));
    assert(engine->loadNote(dangling).audioPath.empty() && "missing file is forgotten");
    assert(engine->identifySpeakers(dangling, 0) == EngineStatus::SpeakersUnavailable);

    // Delete the recording, keep the transcript.
    assert(engine->loadNote(kept).audioPath == recordings + "/recording-1.wav");
    assert(engine->deleteRecording(kept) == EngineStatus::Ok);
    assert(!exists(recordings + "/recording-1.wav"));
    const NoteDetail after = engine->loadNote(kept);
    assert(after.audioPath.empty());
    assert(after.segments.size() == 1 && after.segments[0].text == "keep the words");
    assert(engine->deleteRecording(kept) == EngineStatus::Ok && "idempotent");

    const auto notes = engine->listNotes();
    assert(notes.size() == 2);
    assert(notes[0].id == dangling && "newest first");
    assert(!notes[1].hasAudio && notes[1].preview == "keep the words");

    const std::string shared = engine->exportTranscript(kept, true, false);
    assert(shared.find("keep the words") != std::string::npos);
    assert(engine->exportTranscript(424242, true, false).empty());

    assert(engine->deleteNote(kept) == EngineStatus::Ok);
    assert(engine->loadNote(kept).id == 0);
    assert(engine->listNotes().size() == 1);

    scribaticEngineRelease(engine);
}

} // namespace

int main() {
    std::printf("NoteStoreTests\n");
    testSegmentSpanningTwoSpeakersIsCut();
    testWhisperBoundariesSurvive();
    testIslandWordIsSmoothed();
    testWordInSilenceTakesNearestTurn();
    testTurnFinalWordStaysWithItsSpeaker();
    testNoTurnsLeavesSpeakersUnknown();
    testExportMergesTurnsAndNamesSpeakers();
    testAnonymisedExportCarriesNoNames();
    testSchemaMigratesToCurrentVersion();
    testDeletingANoteLeavesNothingSearchable();
    testSpeakerNamesAreIndexedAndRenamable();
    testRecordingLifecycle();
    std::printf("ok\n");
    return 0;
}
