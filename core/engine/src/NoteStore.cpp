#include "NoteStore.hpp"

#include "TranscriptFormat.hpp"
#include "scribatic/db/Migrations.hpp"
#include "scribatic/db/Queries.hpp"
#include "scribatic/db/Schema.hpp"
#include "sqlite3.h"

#include <utility>

namespace scribatic::core {
namespace q = scribatic::db::queries;

namespace {

/// A prepared statement that finalizes itself. Every query in the store goes
/// through one; there is no string-built SQL anywhere.
class Statement {
public:
    Statement(sqlite3* db, std::string_view sql) {
        if (sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &stmt_,
                               nullptr) != SQLITE_OK) {
            stmt_ = nullptr;
        }
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    [[nodiscard]] bool ok() const { return stmt_ != nullptr; }

    Statement& bind(int index, std::int64_t value) {
        sqlite3_bind_int64(stmt_, index, value);
        return *this;
    }
    Statement& bind(int index, std::int32_t value) {
        sqlite3_bind_int(stmt_, index, value);
        return *this;
    }
    Statement& bind(int index, double value) {
        sqlite3_bind_double(stmt_, index, value);
        return *this;
    }
    Statement& bind(int index, const std::string& value) {
        sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()),
                          SQLITE_TRANSIENT);
        return *this;
    }
    Statement& bindNull(int index) {
        sqlite3_bind_null(stmt_, index);
        return *this;
    }

    /// SQLITE_ROW, SQLITE_DONE, or an error code.
    int step() { return ok() ? sqlite3_step(stmt_) : SQLITE_ERROR; }

    /// Runs a statement that returns no rows, then readies it for reuse.
    bool run() {
        const int rc = step();
        sqlite3_reset(stmt_);
        sqlite3_clear_bindings(stmt_);
        return rc == SQLITE_DONE;
    }

    [[nodiscard]] std::int64_t int64(int column) const {
        return sqlite3_column_int64(stmt_, column);
    }
    [[nodiscard]] std::int32_t int32(int column) const {
        return sqlite3_column_int(stmt_, column);
    }
    [[nodiscard]] double real(int column) const { return sqlite3_column_double(stmt_, column); }
    [[nodiscard]] bool isNull(int column) const {
        return sqlite3_column_type(stmt_, column) == SQLITE_NULL;
    }
    [[nodiscard]] std::string text(int column) const {
        const auto* bytes = sqlite3_column_text(stmt_, column);
        return bytes != nullptr ? reinterpret_cast<const char*>(bytes) : std::string();
    }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

bool exec(sqlite3* db, std::string_view sql) {
    const std::string owned(sql);   // sqlite3_exec needs NUL termination
    return sqlite3_exec(db, owned.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK;
}

/// Rolls back unless committed. Keeps every early return in a write path from
/// leaving a transaction open on the connection.
class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db), open_(exec(db, "BEGIN IMMEDIATE;")) {}
    ~Transaction() {
        if (open_) { exec(db_, "ROLLBACK;"); }
    }
    [[nodiscard]] bool began() const { return open_; }
    bool commit() {
        if (!open_) { return false; }
        open_ = false;
        return exec(db_, "COMMIT;");
    }

private:
    sqlite3* db_;
    bool     open_;
};

std::int32_t userVersion(sqlite3* db) {
    Statement statement(db, "PRAGMA user_version;");
    return statement.step() == SQLITE_ROW ? statement.int32(0) : -1;
}

} // namespace

NoteStore::~NoteStore() { close(); }

EngineStatus NoteStore::open(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ != nullptr) { return EngineStatus::Ok; }

    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(path.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
        sqlite3_close(db_);
        db_ = nullptr;
        return EngineStatus::DatabaseFailed;
    }
    if (!exec(db_, db::kPragmas)) {
        close();
        return EngineStatus::DatabaseFailed;
    }
    const EngineStatus status = migrate();
    if (status != EngineStatus::Ok) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    return status;
}

EngineStatus NoteStore::migrate() {
    Transaction transaction(db_);
    if (!transaction.began()) { return EngineStatus::DatabaseFailed; }

    std::int32_t version = userVersion(db_);
    if (version < 0) { return EngineStatus::DatabaseFailed; }

    if (version == 0) {
        for (const auto statement : db::kBootstrapStatements) {
            if (!exec(db_, statement)) { return EngineStatus::DatabaseFailed; }
        }
        version = 1;
    }
    for (const auto& migration : db::migrations()) {
        if (migration.fromVersion < version) { continue; }
        if (migration.fromVersion != version || !exec(db_, migration.statements)) {
            return EngineStatus::DatabaseFailed;
        }
        version = migration.fromVersion + 1;
    }
    if (version != db::kSchemaVersion) { return EngineStatus::DatabaseFailed; }

    // PRAGMA takes no bound parameters; the value is a compile-time integer.
    const std::string setVersion = "PRAGMA user_version = " + std::to_string(version) + ";";
    if (!exec(db_, setVersion)) { return EngineStatus::DatabaseFailed; }
    return transaction.commit() ? EngineStatus::Ok : EngineStatus::DatabaseFailed;
}

void NoteStore::close() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool NoteStore::isOpen() {
    std::lock_guard<std::mutex> lock(mutex_);
    return db_ != nullptr;
}

std::int64_t NoteStore::insertNote(const std::string& title, std::int64_t createdAt,
                                   std::int64_t durationMs, const std::string& audioName,
                                   const std::vector<TranscriptSegment>& segments,
                                   const std::vector<WordTiming>& words) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) { return 0; }

    Transaction transaction(db_);
    if (!transaction.began()) { return 0; }

    Statement note(db_, q::kInsertNote);
    note.bind(1, title).bind(2, createdAt).bind(3, durationMs).bind(4, std::string("en"));
    if (audioName.empty()) {
        note.bindNull(5);
    } else {
        note.bind(5, audioName);
    }
    if (!note.run()) { return 0; }
    const std::int64_t noteId = sqlite3_last_insert_rowid(db_);

    Statement segment(db_, q::kInsertSegment);
    for (const auto& s : segments) {
        segment.bind(1, noteId).bind(2, s.startMs).bind(3, s.endMs).bind(4, s.text)
            .bind(5, static_cast<double>(s.confidence)).bind(6, s.speaker);
        if (!segment.run()) { return 0; }
    }

    Statement word(db_, q::kInsertWord);
    std::int64_t ordinal = 0;
    for (const auto& w : words) {
        word.bind(1, noteId).bind(2, ordinal++).bind(3, w.segment).bind(4, w.startMs)
            .bind(5, w.endMs).bind(6, w.text).bind(7, static_cast<double>(w.probability));
        if (!word.run()) { return 0; }
    }

    if (rebuildChunks(noteId) != EngineStatus::Ok) { return 0; }
    return transaction.commit() ? noteId : 0;
}

std::vector<NoteSummary> NoteStore::list() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<NoteSummary> out;
    if (db_ == nullptr) { return out; }

    Statement statement(db_, q::kListNotes);
    while (statement.step() == SQLITE_ROW) {
        NoteSummary summary;
        summary.id           = statement.int64(0);
        summary.title        = statement.text(1);
        summary.createdAt    = statement.int64(2);
        summary.durationMs   = statement.int64(3);
        summary.hasAudio     = statement.int32(4) != 0;
        summary.speakerCount = statement.int32(5);
        summary.preview      = statement.text(6);
        out.push_back(std::move(summary));
    }
    return out;
}

NoteDetail NoteStore::load(std::int64_t noteId, const std::string& recordingsDirectory) {
    std::lock_guard<std::mutex> lock(mutex_);
    return loadLocked(noteId, recordingsDirectory);
}

NoteDetail NoteStore::loadLocked(std::int64_t noteId, const std::string& recordingsDirectory) {
    NoteDetail detail;
    if (db_ == nullptr) { return detail; }

    Statement note(db_, q::kLoadNote);
    note.bind(1, noteId);
    if (note.step() != SQLITE_ROW) { return detail; }
    detail.id           = note.int64(0);
    detail.title        = note.text(1);
    detail.createdAt    = note.int64(2);
    detail.durationMs   = note.int64(3);
    if (!note.isNull(4) && !recordingsDirectory.empty()) {
        detail.audioPath = recordingsDirectory + "/" + note.text(4);
    }
    detail.speakerCount = note.int32(5);

    Statement speakers(db_, q::kLoadSpeakers);
    speakers.bind(1, noteId);
    while (speakers.step() == SQLITE_ROW) {
        SpeakerLabel label;
        label.index = speakers.int32(0);
        label.name  = speakers.text(1);
        detail.speakers.push_back(std::move(label));
    }
    resolveDisplayNames(detail.speakers);

    Statement segments(db_, q::kLoadSegments);
    segments.bind(1, noteId);
    while (segments.step() == SQLITE_ROW) {
        TranscriptSegment segment;
        segment.startMs    = segments.int64(0);
        segment.endMs      = segments.int64(1);
        segment.text       = segments.text(2);
        segment.confidence = static_cast<float>(segments.real(3));
        segment.speaker    = segments.int32(4);
        segment.isFinal    = true;
        detail.segments.push_back(std::move(segment));
    }
    return detail;
}

std::vector<WordTiming> NoteStore::words(std::int64_t noteId) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<WordTiming> out;
    if (db_ == nullptr) { return out; }

    Statement statement(db_, q::kLoadWords);
    statement.bind(1, noteId);
    while (statement.step() == SQLITE_ROW) {
        WordTiming word;
        word.segment     = statement.int32(0);
        word.startMs     = statement.int64(1);
        word.endMs       = statement.int64(2);
        word.text        = statement.text(3);
        word.probability = static_cast<float>(statement.real(4));
        out.push_back(std::move(word));
    }
    return out;
}

EngineStatus NoteStore::replaceTranscript(std::int64_t noteId,
                                          const std::vector<TranscriptSegment>& segments,
                                          std::int32_t speakerCount) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) { return EngineStatus::DatabaseFailed; }

    Transaction transaction(db_);
    if (!transaction.began()) { return EngineStatus::DatabaseFailed; }

    for (const auto sql : {q::kDeleteSegments, q::kDeleteSpeakers}) {
        Statement clear(db_, sql);
        if (!clear.bind(1, noteId).run()) { return EngineStatus::DatabaseFailed; }
    }

    Statement segment(db_, q::kInsertSegment);
    for (const auto& s : segments) {
        segment.bind(1, noteId).bind(2, s.startMs).bind(3, s.endMs).bind(4, s.text)
            .bind(5, static_cast<double>(s.confidence)).bind(6, s.speaker);
        if (!segment.run()) { return EngineStatus::DatabaseFailed; }
    }

    Statement speaker(db_, q::kInsertSpeaker);
    for (std::int32_t index = 0; index < speakerCount; ++index) {
        if (!speaker.bind(1, noteId).bind(2, index).run()) {
            return EngineStatus::DatabaseFailed;
        }
    }

    Statement count(db_, q::kUpdateSpeakerCount);
    if (!count.bind(1, speakerCount).bind(2, noteId).run()) {
        return EngineStatus::DatabaseFailed;
    }

    if (rebuildChunks(noteId) != EngineStatus::Ok) { return EngineStatus::DatabaseFailed; }
    return transaction.commit() ? EngineStatus::Ok : EngineStatus::DatabaseFailed;
}

EngineStatus NoteStore::renameSpeaker(std::int64_t noteId, std::int32_t speaker,
                                      const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) { return EngineStatus::DatabaseFailed; }

    Transaction transaction(db_);
    if (!transaction.began()) { return EngineStatus::DatabaseFailed; }

    Statement rename(db_, q::kRenameSpeaker);
    if (!rename.bind(1, name).bind(2, noteId).bind(3, speaker).run() ||
        sqlite3_changes(db_) != 1) {
        return EngineStatus::DatabaseFailed;
    }
    // Names are part of the indexed text, so a rename re-indexes the note.
    if (rebuildChunks(noteId) != EngineStatus::Ok) { return EngineStatus::DatabaseFailed; }
    return transaction.commit() ? EngineStatus::Ok : EngineStatus::DatabaseFailed;
}

std::string NoteStore::audioName(std::int64_t noteId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) { return {}; }
    Statement statement(db_, q::kNoteAudio);
    statement.bind(1, noteId);
    return statement.step() == SQLITE_ROW ? statement.text(0) : std::string();
}

EngineStatus NoteStore::clearAudio(std::int64_t noteId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) { return EngineStatus::DatabaseFailed; }
    Statement statement(db_, q::kClearAudio);
    return statement.bind(1, noteId).run() ? EngineStatus::Ok : EngineStatus::DatabaseFailed;
}

EngineStatus NoteStore::deleteNote(std::int64_t noteId, std::string* audioName) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) { return EngineStatus::DatabaseFailed; }

    Transaction transaction(db_);
    if (!transaction.began()) { return EngineStatus::DatabaseFailed; }

    Statement audio(db_, q::kNoteAudio);
    audio.bind(1, noteId);
    const std::string name = audio.step() == SQLITE_ROW ? audio.text(0) : std::string();

    Statement remove(db_, q::kDeleteNoteCascade);
    if (!remove.bind(1, noteId).run()) { return EngineStatus::DatabaseFailed; }
    if (!transaction.commit()) { return EngineStatus::DatabaseFailed; }

    if (audioName != nullptr) { *audioName = name; }
    return EngineStatus::Ok;
}

std::vector<NoteStore::AudioReference> NoteStore::audioReferences() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AudioReference> out;
    if (db_ == nullptr) { return out; }
    Statement statement(db_, q::kReferencedAudio);
    while (statement.step() == SQLITE_ROW) {
        out.push_back({statement.int64(0), statement.text(1)});
    }
    return out;
}

EngineStatus NoteStore::rebuildChunks(std::int64_t noteId) {
    Statement clear(db_, q::kDeleteChunks);
    if (!clear.bind(1, noteId).run()) { return EngineStatus::DatabaseFailed; }

    const NoteDetail detail = loadLocked(noteId, {});
    Statement insert(db_, q::kInsertChunk);
    std::int32_t ordinal = 0;
    for (const auto& chunk : buildChunks(detail)) {
        if (!insert.bind(1, noteId).bind(2, ordinal++).bind(3, chunk).run()) {
            return EngineStatus::DatabaseFailed;
        }
    }
    return EngineStatus::Ok;
}

} // namespace scribatic::core
