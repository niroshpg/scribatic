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
#include "scribatic/db/Migrations.hpp"
#include "scribatic/db/Schema.hpp"
#include "sqlite3.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
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

/// A whole turn in one segment (a speaker re-transcribed in their own
/// language, or a long correction) still makes passages that fit.
void testLongSegmentIsCutIntoPassages() {
    NoteDetail note = sampleNote();
    std::string turn;
    for (int i = 0; turn.size() < 3000; ++i) { turn += "word" + std::to_string(i) + " "; }
    turn.pop_back();
    auto long_ = segment(5000, 65000, turn.c_str());
    long_.speaker = 1;
    note.segments = {long_};
    const auto passages = buildPassages(note, 700);
    assert(passages.size() >= 5);
    std::string joined;
    std::int64_t last = 5000;
    for (const auto& passage : passages) {
        assert(passage.text.size() <= 700 + std::string("Speaker 2: ").size());
        assert(passage.text.find("Speaker 2: ") == 0);
        assert(passage.startMs >= last && passage.endMs > passage.startMs && passage.endMs <= 65000);
        last = passage.endMs;
        joined += passage.text.substr(std::string("Speaker 2: ").size()) + " ";
    }
    joined.pop_back();
    assert(joined == turn && "no word lost or cut");
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

/// A store from before schema 5: its chunks get times when it is opened.
void testUpgradeGivesChunksTheirTimes() {
    const std::string dir = tempDirectory();
    const std::string db = dir + "/notes.sqlite";
    {
        sqlite3* raw = nullptr;
        assert(sqlite3_open(db.c_str(), &raw) == SQLITE_OK);
        for (const auto statement : scribatic::db::kBootstrapStatements) {
            assert(sqlite3_exec(raw, std::string(statement).c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
        }
        for (const auto& migration : scribatic::db::migrations()) {
            if (migration.fromVersion >= 4) { break; }
            assert(sqlite3_exec(raw, std::string(migration.statements).c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
        }
        const char* rows =
            "INSERT INTO notes(id, title, created_at, duration_ms) VALUES (1, 'Old', 1700000000, 9000);"
            "INSERT INTO segments(note_id, start_ms, end_ms, text) VALUES (1, 1500, 4000, 'kept from before');"
            "INSERT INTO chunks(note_id, ordinal, text) VALUES (1, 0, 'kept from before');"
            "PRAGMA user_version = 4;";
        assert(sqlite3_exec(raw, rows, nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(raw);
    }
    NoteStore store;
    assert(store.open(db) == EngineStatus::Ok);
    const auto passages = store.passages(false);
    assert(passages.size() == 1 && passages[0].startMs == 1500 && passages[0].endMs == 4000);
    assert(passages[0].embedding.empty());
    assert(store.searchWords("before", 5).size() == 1);
}

/// The word half of search, and the passages the meaning half is stored on.
void testSearchingAcrossNotes() {
    const std::string dir = tempDirectory();
    NoteStore store;
    assert(store.open(dir + "/notes.sqlite") == EngineStatus::Ok);
    TranscriptSegment budget = segment(12000, 15000, "We agreed the budget for the excursion");
    budget.speaker = 0;
    const std::int64_t planning = store.insertNote("Planning", 1700000000, 20000, "",
        {segment(0, 3000, "Welcome everyone"), budget}, {});
    assert(store.replaceTranscript(planning, {segment(0, 3000, "Welcome everyone"), budget}, 1) == EngineStatus::Ok);
    assert(store.renameSpeaker(planning, 0, "Sam") == EngineStatus::Ok);
    std::vector<TranscriptSegment> monologue;
    for (int k = 0; k < 30; ++k) {
        monologue.push_back(segment(k * 4000, k * 4000 + 4000,
            "A sentence of about a hundred characters, which is roughly how long a whisper segment runs on."));
    }
    const std::int64_t lecture = store.insertNote("Lecture", 1700001000, 120000, "", monologue, {});

    // Passages: each with its times and speakers; a long turn split into several.
    auto passages = store.passages(false);
    const auto ofNote = [&](std::int64_t id) {
        std::vector<Passage> out;
        for (const auto& p : passages) { if (p.noteId == id) { out.push_back(p); } }
        return out;
    };
    assert(ofNote(planning).size() == 1);
    assert(ofNote(planning)[0].text.find("Sam: We agreed the budget") != std::string::npos);
    assert(ofNote(lecture).size() > 3 && "a long monologue becomes several passages");
    for (const auto& p : ofNote(lecture)) { assert(p.text.size() <= 800 && p.endMs > p.startMs); }
    assert(ofNote(lecture)[1].startMs > 0);

    // Words: prefixes, speaker names, and what the user typed is never FTS5 syntax.
    auto hits = store.searchWords("budg", 5);
    assert(hits.size() == 1 && hits[0].noteId == planning && hits[0].startMs == 0);
    assert(store.searchWords("Sam excursion", 5).front().noteId == planning);
    assert(store.searchWords("\"budget OR * NEAR(", 5).size() == 1);
    assert(store.searchWords("   ", 5).empty());

    // Meaning vectors are stored with passages and cleared when the note changes.
    const auto pending = store.passages(true);
    assert(pending.size() == passages.size());
    assert(store.setPassageEmbedding(ofNote(planning)[0].id, {0.5F, -0.25F, 1.0F}) == EngineStatus::Ok);
    assert(store.passages(true).size() == passages.size() - 1);
    passages = store.passages(false);
    assert((ofNote(planning)[0].embedding == Embedding{0.5F, -0.25F, 1.0F}));
    assert(store.renameSpeaker(planning, 0, "Samira") == EngineStatus::Ok);
    assert(store.passages(true).size() == passages.size() && "a changed note is embedded again");
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

/// The user corrects speakers: one block to another speaker or a new one,
/// several speakers merged. Numbers stay 0..n-1 and names stay with people.
void testSpeakersCanBeCorrected() {
    const std::string dir = tempDirectory();
    NoteStore store;
    assert(store.open(dir + "/notes.sqlite") == EngineStatus::Ok);
    const std::int64_t id = store.insertNote("Chat", 1700000000, 6000, "",
                                             {segment(0, 1000, "a"), segment(1000, 2000, "b"),
                                              segment(2000, 3000, "c"), segment(3000, 4000, "d")}, {});
    // Four blocks over three speakers, as an over-eager estimate might give.
    auto s = [](std::int64_t start, const char* text, std::int32_t speaker) {
        auto out = segment(start, start + 1000, text); out.speaker = speaker; return out;
    };
    assert(store.replaceTranscript(id, {s(0, "a", 0), s(1000, "b", 1), s(2000, "c", 0), s(3000, "d", 2)}, 3) == EngineStatus::Ok);
    assert(store.renameSpeaker(id, 0, "Ana") == EngineStatus::Ok);
    NoteDetail note = store.load(id, dir);
    assert(note.speakerCount == 3 && note.segments[1].id > 0 && "segments carry their ids");

    // Speaker 3's only block goes to Speaker 2: speaker 3 is gone, 2 remain.
    std::int32_t landed = -1;
    assert(store.setSegmentSpeaker(id, note.segments[3].id, 1, &landed) == EngineStatus::Ok);
    note = store.load(id, dir);
    assert(note.speakerCount == 2 && landed == 1);
    assert(note.segments[3].speaker == 1 && note.speakers[0].displayName == "Ana");

    // One of Ana's blocks becomes someone new: a third speaker again.
    assert(store.setSegmentSpeaker(id, note.segments[2].id, -1, &landed) == EngineStatus::Ok);
    note = store.load(id, dir);
    assert(note.speakerCount == 3 && landed == 2 && note.segments[2].speaker == 2);
    assert(note.speakers[2].displayName == "Speaker 3");

    // Merge the new one and speaker 2 into Ana: one speaker, still Ana.
    assert(store.mergeSpeakers(id, {1, 2}, 0) == EngineStatus::Ok);
    note = store.load(id, dir);
    assert(note.speakerCount == 1 && note.speakers.size() == 1 && note.speakers[0].displayName == "Ana");
    for (const auto& segment : note.segments) { assert(segment.speaker == 0); }

    // Merging into a later speaker renumbers it to 0.
    assert(store.setSegmentSpeaker(id, note.segments[0].id, -1, &landed) == EngineStatus::Ok);
    assert(store.mergeSpeakers(id, {0}, 1) == EngineStatus::Ok);
    note = store.load(id, dir);
    assert(note.speakerCount == 1 && note.segments[0].speaker == 0 && note.segments[3].speaker == 0);
}

void testLayoutAndRefinedPreview() {
    const std::string dir = tempDirectory();
    NoteStore store;
    assert(store.open(dir + "/notes.sqlite") == EngineStatus::Ok);
    const std::int64_t id = store.insertNote("Talk", 1700000000, 4000, "", {segment(0, 2000, "preview")},
                                             {word(0, 0, 2000, " preview")});
    NoteDetail note = store.load(id, dir);
    assert(note.layout == "auto" && !note.refined);
    assert(store.setLayout(id, "lecture") == EngineStatus::Ok);
    assert(store.load(id, dir).layout == "lecture");

    assert(store.replaceWithRefined(id, {segment(0, 1500, "final one"), segment(1500, 3000, "final two")},
                                    {word(0, 0, 1500, " final"), word(1, 1500, 3000, " two")}) == EngineStatus::Ok);
    note = store.load(id, dir);
    assert(note.refined && note.segments.size() == 2 && note.segments[0].text == "final one");
    assert(note.speakerCount == 0 && "speakers are identified again from the new words");
    assert(store.words(id).size() == 2 && store.words(id)[1].text == " two");
    assert(note.layout == "lecture" && "the user's layout survives refinement");
    assert(note.language == "en");

    assert(store.replaceWithRefined(id, {segment(0, 1500, "final one")}, {}, "si") == EngineStatus::Ok);
    assert(store.load(id, dir).language == "si" && "a detected language becomes the note's");
}

/// A card's correction: its words in the first segment, the rest emptied, the
/// recognised text kept until restored. Exports, search and the summary flag
/// all follow the correction.
void testCorrectingACard() {
    const std::string dir = tempDirectory();
    const std::string db = dir + "/notes.sqlite";
    NoteStore store;
    assert(store.open(db) == EngineStatus::Ok);
    const std::int64_t id = store.insertNote("Call", 1700000000, 6000, "",
        {segment(0, 2000, "we use quick books zero"), segment(2000, 4000, "for invoices"),
         segment(4000, 6000, "and that is it")}, {});
    NoteDetail note = store.load(id, dir);
    const std::vector<std::int64_t> card{note.segments[0].id, note.segments[1].id};
    assert(!note.segments[0].edited && note.segments[0].original.empty());

    assert(store.setSummary(id, "They use QuickBooks.") == EngineStatus::Ok);
    assert(!store.load(id, dir).summaryStale);

    assert(store.editText(id, card, "We use QuickBooks and Xero for invoices.") == EngineStatus::Ok);
    note = store.load(id, dir);
    assert(note.segments.size() == 3 && "segments keep their times and audio");
    assert(note.segments[0].text == "We use QuickBooks and Xero for invoices.");
    assert(note.segments[0].edited && note.segments[0].original == "we use quick books zero");
    assert(note.segments[1].text.empty() && note.segments[1].original == "for invoices");
    assert(note.segments[0].startMs == 0 && note.segments[1].endMs == 4000);
    assert(!note.segments[2].edited);
    assert(note.summaryStale && "the summary no longer matches");

    const std::string shared = formatTranscript(note, {false, false});
    assert(shared.find("QuickBooks and Xero") != std::string::npos);
    assert(shared.find("quick books zero") == std::string::npos && "exports use the correction");
    assert(shared.find("  ") == std::string::npos && "an emptied segment leaves no gap");
    assert(scalar(db, "SELECT COUNT(*) FROM chunks_fts WHERE chunks_fts MATCH 'xero'") == 1);
    assert(scalar(db, "SELECT COUNT(*) FROM chunks_fts WHERE chunks_fts MATCH 'zero'") == 0);
    assert(store.list()[0].preview.find("Xero") != std::string::npos);

    // A second correction keeps the first original, not the first correction.
    assert(store.editText(id, card, "We use Xero for invoices.") == EngineStatus::Ok);
    assert(store.load(id, dir).segments[0].original == "we use quick books zero");

    assert(store.setSummary(id, "They use Xero.") == EngineStatus::Ok);
    assert(!store.load(id, dir).summaryStale && "a new summary is current again");

    assert(store.restoreText(id, card) == EngineStatus::Ok);
    note = store.load(id, dir);
    assert(note.segments[0].text == "we use quick books zero" && !note.segments[0].edited);
    assert(note.segments[1].text == "for invoices" && note.segments[1].original.empty());
    assert(note.summaryStale);
    assert(scalar(db, "SELECT COUNT(*) FROM chunks_fts WHERE chunks_fts MATCH 'xero'") == 0);
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
    assert(engine->identifySpeakers(dangling, 0, "") == EngineStatus::SpeakersUnavailable);

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

/// A mono 16 kHz float32 WAV as a writer leaves it when the app dies before
/// closing it: RIFF and `data` sizes still 0, the last sample half written.
void writeUnfinishedWav(const std::string& path, std::size_t samples) {
    const auto u32 = [](std::uint32_t v) { return std::string(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [](std::uint16_t v) { return std::string(reinterpret_cast<const char*>(&v), 2); };
    std::string bytes = "RIFF" + u32(0) + "WAVE";
    bytes += "fmt " + u32(16) + u16(3) + u16(1) + u32(16000) + u32(64000) + u16(4) + u16(32);
    bytes += "data" + u32(0);
    for (std::size_t i = 0; i < samples; ++i) {
        const float sample = 0.25F * static_cast<float>(i % 32) / 32.0F;
        bytes.append(reinterpret_cast<const char*>(&sample), 4);
    }
    bytes += "\x01\x02";   // a torn last sample
    touch(path, bytes);
}

std::uint32_t u32At(const std::string& path, std::streamoff offset) {
    std::ifstream file(path, std::ios::binary);
    file.seekg(offset);
    std::uint32_t value = 0;
    file.read(reinterpret_cast<char*>(&value), 4);
    return value;
}

/// A recording the app was killed during is kept as a note rather than swept.
void testUnfinishedRecordingIsRecovered() {
    const std::string dir = tempDirectory();
    const std::string recordings = dir + "/recordings";
    assert(::mkdir(recordings.c_str(), 0700) == 0);
    touch(dir + "/whisper.bin", "not a model");
    touch(dir + "/llama.gguf", "not a model");

    const std::string cut = recordings + "/recording-1790000000123.wav";
    writeUnfinishedWav(cut, 40000);                                   // 2.5 s
    writeUnfinishedWav(recordings + "/recording-1790000100000.wav", 8000);   // 0.5 s
    touch(recordings + "/recording-1790000200000.wav");                 // not audio

    EngineConfig config;
    config.whisperModelPath = dir + "/whisper.bin";
    config.llamaModelPath = dir + "/llama.gguf";
    config.databasePath = dir + "/notes.sqlite";
    config.recordingsDirectory = recordings;
    const auto start = [&config] {
        EngineStatus status = EngineStatus::Ok;
        EngineInterface* engine = EngineInterface::create(config, &status);
        assert(engine != nullptr);
        assert(engine->warmUp() == EngineStatus::ModelLoadFailed);   // after the sweep
        return engine;
    };

    EngineInterface* engine = start();
    const auto notes = engine->listNotes();
    assert(notes.size() == 1 && "only the recording worth keeping is recovered");
    assert(notes[0].title == "Recovered recording" && notes[0].hasAudio);
    assert(notes[0].createdAt == 1790000000 && "started when its name says");
    assert(notes[0].durationMs == 2500);
    const NoteDetail note = engine->loadNote(notes[0].id);
    assert(note.segments.empty() && note.language == "auto" && !note.refined);
    assert(note.audioPath == cut);
    assert(!exists(recordings + "/recording-1790000100000.wav") && "too short to keep");
    assert(!exists(recordings + "/recording-1790000200000.wav") && "not audio");

    struct stat info {};
    assert(::stat(cut.c_str(), &info) == 0 && info.st_size == 44 + 160000 && "torn sample dropped");
    assert(u32At(cut, 4) == 36 + 160000 && u32At(cut, 40) == 160000 && "sizes written");

    // Deleted, it stays deleted: the file goes with the note, so the next
    // launch has nothing to recover.
    assert(engine->deleteNote(note.id) == EngineStatus::Ok);
    assert(!exists(cut));
    scribaticEngineRelease(engine);
    engine = start();
    assert(engine->listNotes().empty());
    scribaticEngineRelease(engine);
}

/// Audio from elsewhere, converted by the app: a note with no transcript yet,
/// in the language setting, for the accurate pass to transcribe.
void testImportedRecordingBecomesANote() {
    const std::string dir = tempDirectory();
    const std::string recordings = dir + "/recordings";
    assert(::mkdir(recordings.c_str(), 0700) == 0);
    touch(dir + "/whisper.bin", "not a model");
    touch(dir + "/llama.gguf", "not a model");
    EngineConfig config;
    config.whisperModelPath = dir + "/whisper.bin";
    config.llamaModelPath = dir + "/llama.gguf";
    config.databasePath = dir + "/notes.sqlite";
    config.recordingsDirectory = recordings;
    EngineStatus status = EngineStatus::Ok;
    EngineInterface* engine = EngineInterface::create(config, &status);
    assert(engine != nullptr && engine->warmUp() == EngineStatus::ModelLoadFailed);

    const std::string imported = recordings + "/recording-1790000300000.wav";
    writeUnfinishedWav(imported, 48000);   // 3 s
    engine->setLanguage("es");
    const std::int64_t id = engine->importRecording(imported, "Interview.m4a", 1790000400);
    assert(id > 0);
    const NoteDetail note = engine->loadNote(id);
    assert(note.title == "Interview.m4a" && note.createdAt == 1790000400);
    assert(note.durationMs == 3000 && note.audioPath == imported);
    assert(note.segments.empty() && !note.refined && note.language == "es");

    engine->setLanguage("auto");
    const std::string second = recordings + "/recording-1790000500000.wav";
    writeUnfinishedWav(second, 16000);
    assert(engine->loadNote(engine->importRecording(second, "Memo", 1)).language == "auto");

    // Only the apps' own recordings, in their own directory.
    touch(dir + "/recording-1.wav");
    assert(engine->importRecording(dir + "/recording-1.wav", "Elsewhere", 1) == 0);
    touch(recordings + "/notes.wav");
    assert(engine->importRecording(recordings + "/notes.wav", "Misnamed", 1) == 0);
    touch(recordings + "/recording-2.wav", "not audio");
    assert(engine->importRecording(recordings + "/recording-2.wav", "Not audio", 1) == 0);
    scribaticEngineRelease(engine);
}

/// A backup restores into another install with everything the note had,
/// recordings under new names; restoring it again adds nothing.
void testBackupRestoresElsewhere() {
    const auto install = [](const std::string& dir) {
        const std::string recordings = dir + "/recordings";
        ::mkdir(recordings.c_str(), 0700);
        touch(dir + "/whisper.bin", "not a model");
        touch(dir + "/llama.gguf", "not a model");
        EngineConfig config;
        config.whisperModelPath = dir + "/whisper.bin";
        config.llamaModelPath = dir + "/llama.gguf";
        config.databasePath = dir + "/notes.sqlite";
        config.recordingsDirectory = recordings;
        EngineStatus status = EngineStatus::Ok;
        EngineInterface* engine = EngineInterface::create(config, &status);
        assert(engine != nullptr && engine->warmUp() == EngineStatus::ModelLoadFailed);
        return engine;
    };
    const std::string here = tempDirectory();
    EngineInterface* source = install(here);
    const std::string recording = here + "/recordings/recording-1790000600000.wav";
    writeUnfinishedWav(recording, 32000);
    const std::int64_t id = source->importRecording(recording, "Standup", 1790000600);
    assert(id > 0);
    // A transcript, a correction, a named speaker and a summary to carry over.
    {
        NoteStore store;
        assert(store.open(here + "/notes.sqlite") == EngineStatus::Ok);
        assert(store.replaceTranscript(id, {segment(0, 1000, "hello from quick books"), segment(1000, 2000, "bye")}, 1)
               == EngineStatus::Ok);
    }
    NoteDetail note = source->loadNote(id);
    assert(source->editText(id, {note.segments[0].id}, "Hello from QuickBooks") == EngineStatus::Ok);
    assert(source->renameSpeaker(id, 0, "Ana") == EngineStatus::Ok);
    assert(source->setNoteSummary(id, "Ana said hello.") == EngineStatus::Ok);

    const std::string unpacked = tempDirectory();
    assert(source->writeBackup(unpacked + "/notes.sqlite") == EngineStatus::Ok);
    ::mkdir((unpacked + "/recordings").c_str(), 0700);
    std::rename(recording.c_str(), (unpacked + "/recordings/recording-1790000600000.wav").c_str());
    scribaticEngineRelease(source);

    EngineInterface* target = install(tempDirectory());
    RestoreResult result = target->restoreBackup(unpacked + "/notes.sqlite", unpacked + "/recordings");
    assert(result.status == EngineStatus::Ok && result.restored == 1 && result.skipped == 0);
    const auto notes = target->listNotes();
    assert(notes.size() == 1 && notes[0].title == "Standup" && notes[0].hasAudio);
    note = target->loadNote(notes[0].id);
    assert(note.segments.size() == 2 && note.segments[0].text == "Hello from QuickBooks");
    assert(note.segments[0].edited && note.segments[0].original == "hello from quick books");
    assert(note.speakers.size() == 1 && note.speakers[0].name == "Ana");
    assert(note.summary == "Ana said hello." && !note.summaryStale);
    assert(exists(note.audioPath) && note.audioPath.find("recording-1790000600000") == std::string::npos);
    assert(!exists(unpacked + "/recordings/recording-1790000600000.wav") && "moved, not copied");
    assert(target->exportTranscript(note.id, false, false).find("QuickBooks") != std::string::npos);

    result = target->restoreBackup(unpacked + "/notes.sqlite", unpacked + "/recordings");
    assert(result.status == EngineStatus::Ok && result.restored == 0 && result.skipped == 1);
    assert(target->listNotes().size() == 1 && "nothing is duplicated");
    scribaticEngineRelease(target);
}

// -- Model catalog --------------------------------------------------------------------

void testCatalogPinsEveryModel() {
    const auto catalog = modelCatalog();
    assert(catalog.size() == 7);
    int optional = 0;
    for (const auto& model : catalog) {
        assert(!model.fileName.empty() && !model.title.empty());
        assert(model.sizeBytes > 0);
        assert(model.sha256.size() == 64);
        assert(model.sha256.find_first_not_of("0123456789abcdef") == std::string::npos);
        if (!model.partOf.empty()) {
            // A later part of a split model: its first part is in the list,
            // and it is kept or left out with it.
            const auto first = std::find_if(catalog.begin(), catalog.end(),
                                            [&](const ModelSpec& m) { return m.fileName == model.partOf; });
            assert(first != catalog.end() && first->partOf.empty() && first->required == model.required);
            assert(model.sizeBytes <= 1500LL * 1000 * 1000 && "each part fits one Play pack");
            continue;
        }
        assert(!model.purpose.empty());
        if (!model.required) {
            ++optional;
            assert(!model.withoutIt.empty() && "an optional model must say what is lost");
        }
    }
    assert(optional == 1 && "only the instruct model may be left out");
}

void testEngineStartsWithoutTheInstructModel() {
    const std::string dir = tempDirectory();
    touch(dir + "/whisper.bin", "not a model");
    EngineConfig config;
    config.whisperModelPath = dir + "/whisper.bin";
    config.llamaModelPath = dir + "/absent.gguf";
    config.databasePath = dir + "/notes.sqlite";
    EngineStatus status = EngineStatus::Ok;
    EngineInterface* engine = EngineInterface::create(config, &status);
    assert(engine != nullptr && status == EngineStatus::Ok);
    scribaticEngineRelease(engine);

    config.whisperModelPath = dir + "/absent.bin";
    assert(EngineInterface::create(config, &status) == nullptr);
    assert(status == EngineStatus::ModelNotFound);
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
    testLongSegmentIsCutIntoPassages();
    testSchemaMigratesToCurrentVersion();
    testUpgradeGivesChunksTheirTimes();
    testSearchingAcrossNotes();
    testDeletingANoteLeavesNothingSearchable();
    testSpeakerNamesAreIndexedAndRenamable();
    testSpeakersCanBeCorrected();
    testLayoutAndRefinedPreview();
    testCorrectingACard();
    testRecordingLifecycle();
    testUnfinishedRecordingIsRecovered();
    testImportedRecordingBecomesANote();
    testBackupRestoresElsewhere();
    testCatalogPinsEveryModel();
    testEngineStartsWithoutTheInstructModel();
    std::printf("ok\n");
    return 0;
}
