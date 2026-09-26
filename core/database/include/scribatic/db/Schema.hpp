// =============================================================================
//  Schema.hpp — Single source of truth for the on-device store.
//
//  The DDL lives in C++ (not in Swift Core Data and not in Android Room) so the
//  two apps cannot drift. Each platform opens the same file format with the
//  same page size, and a database produced on one is byte-compatible with the
//  other — which keeps an eventual encrypted local export/import path honest.
//
//  Vector search is provided by sqlite-vss (faiss-backed). Chunk embeddings are
//  384-dimensional (MiniLM-class), quantised to float32 on write.
// =============================================================================
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace scribatic::db {

/// Bumped on every migration; compared against `PRAGMA user_version`.
inline constexpr std::int32_t kSchemaVersion   = 2;
inline constexpr std::int32_t kEmbeddingDims   = 384;
inline constexpr std::int32_t kChunkTokenSize  = 256;
inline constexpr std::int32_t kChunkTokenStride= 64;   ///< overlap for recall

/// Applied once at open, before any statement executes.
inline constexpr std::string_view kPragmas = R"SQL(
PRAGMA journal_mode = WAL;          -- concurrent reader while the engine writes
PRAGMA synchronous  = NORMAL;       -- WAL makes FULL unnecessary for our risk
PRAGMA temp_store   = MEMORY;
PRAGMA mmap_size    = 268435456;    -- 256 MiB of the DB mapped, not heap-read
PRAGMA foreign_keys = ON;
)SQL";

inline constexpr std::string_view kCreateNotes = R"SQL(
CREATE TABLE IF NOT EXISTS notes (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    title        TEXT    NOT NULL DEFAULT '',
    created_at   INTEGER NOT NULL,          -- unix epoch, seconds
    duration_ms  INTEGER NOT NULL DEFAULT 0,
    locale       TEXT    NOT NULL DEFAULT 'en',
    summary      TEXT,                      -- llama.cpp output, nullable
    audio_path   TEXT                       -- app-private container only
);
)SQL";

inline constexpr std::string_view kCreateSegments = R"SQL(
CREATE TABLE IF NOT EXISTS segments (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    note_id     INTEGER NOT NULL REFERENCES notes(id) ON DELETE CASCADE,
    start_ms    INTEGER NOT NULL,
    end_ms      INTEGER NOT NULL,
    text        TEXT    NOT NULL,
    confidence  REAL    NOT NULL DEFAULT 0.0
);
CREATE INDEX IF NOT EXISTS idx_segments_note ON segments(note_id, start_ms);
)SQL";

inline constexpr std::string_view kCreateChunks = R"SQL(
CREATE TABLE IF NOT EXISTS chunks (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    note_id   INTEGER NOT NULL REFERENCES notes(id) ON DELETE CASCADE,
    ordinal   INTEGER NOT NULL,
    text      TEXT    NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_chunks_note ON chunks(note_id, ordinal);
)SQL";

/// Virtual table backing approximate k-NN retrieval. `rowid` is joined back to
/// `chunks.id`, so the index stores vectors only — never plaintext.
///
/// NOT part of the bootstrap. `vss0` is a loadable extension, and nothing
/// loads it yet: executing this without it fails with "no such module" and
/// takes the whole bootstrap transaction down with it. It is created, along
/// with a delete trigger mirroring kCreateFtsTriggers, when embedding lands.
inline constexpr std::string_view kCreateVectorIndex = R"SQL(
CREATE VIRTUAL TABLE IF NOT EXISTS vss_chunks USING vss0(
    embedding(384)
);
)SQL";

/// Lexical fallback. Hybrid retrieval (FTS5 BM25 + VSS distance, reciprocal
/// rank fusion) beats either signal alone on short, keyword-heavy queries.
inline constexpr std::string_view kCreateFts = R"SQL(
CREATE VIRTUAL TABLE IF NOT EXISTS chunks_fts USING fts5(
    text,
    content = 'chunks',
    content_rowid = 'id',
    tokenize = 'porter unicode61'
);
)SQL";

/// Ordered bootstrap sequence executed inside a single transaction. Creates
/// the version 1 schema; `Migrations.hpp` carries it forward from there, so a
/// fresh install and an upgraded one run the same statements.
inline constexpr std::array<std::string_view, 4> kBootstrapStatements{
    kCreateNotes,
    kCreateSegments,
    kCreateChunks,
    kCreateFts,
};

// -- Version 2 ---------------------------------------------------------------

/// Speakers are per note and carry only a display name. There is deliberately
/// no embedding column: a voiceprint that outlives the recording would be
/// biometric data about whoever spoke, and none is ever stored (ADR-009).
inline constexpr std::string_view kCreateSpeakers = R"SQL(
CREATE TABLE IF NOT EXISTS speakers (
    note_id  INTEGER NOT NULL REFERENCES notes(id) ON DELETE CASCADE,
    idx      INTEGER NOT NULL,
    name     TEXT    NOT NULL DEFAULT '',
    PRIMARY KEY (note_id, idx)
) WITHOUT ROWID;
)SQL";

/// Word-level timings as whisper produced them. Segments are re-cut from these
/// whenever speakers are identified, so identification can be re-run with a
/// different speaker count long after the recording session has gone. They are
/// transcript text at a finer grain, and are deleted with the note.
inline constexpr std::string_view kCreateWords = R"SQL(
CREATE TABLE IF NOT EXISTS words (
    note_id     INTEGER NOT NULL REFERENCES notes(id) ON DELETE CASCADE,
    ordinal     INTEGER NOT NULL,
    segment     INTEGER NOT NULL,           -- whisper segment it came from
    start_ms    INTEGER NOT NULL,
    end_ms      INTEGER NOT NULL,
    text        TEXT    NOT NULL,
    probability REAL    NOT NULL DEFAULT 0.0,
    PRIMARY KEY (note_id, ordinal)
) WITHOUT ROWID;
)SQL";

inline constexpr std::string_view kAddSegmentSpeaker =
    "ALTER TABLE segments ADD COLUMN speaker INTEGER NOT NULL DEFAULT -1;";

inline constexpr std::string_view kAddNoteSpeakerCount =
    "ALTER TABLE notes ADD COLUMN speaker_count INTEGER NOT NULL DEFAULT 0;";

/// Keeps the FTS5 index in step with `chunks`.
///
/// `chunks_fts` is an external-content table joined by rowid, and a virtual
/// table cannot be the child of a foreign key. So `ON DELETE CASCADE` stops at
/// `chunks`: without these triggers, deleting a note removed its chunks and
/// left their text searchable in the index — the exact defect the cascade was
/// meant to rule out. SQLite fires row triggers for cascaded deletes too, which
/// is what makes deleting the note sufficient.
inline constexpr std::string_view kCreateFtsTriggers = R"SQL(
CREATE TRIGGER IF NOT EXISTS chunks_fts_after_insert AFTER INSERT ON chunks BEGIN
    INSERT INTO chunks_fts(rowid, text) VALUES (new.id, new.text);
END;
CREATE TRIGGER IF NOT EXISTS chunks_fts_after_delete AFTER DELETE ON chunks BEGIN
    INSERT INTO chunks_fts(chunks_fts, rowid, text) VALUES ('delete', old.id, old.text);
END;
CREATE TRIGGER IF NOT EXISTS chunks_fts_after_update AFTER UPDATE ON chunks BEGIN
    INSERT INTO chunks_fts(chunks_fts, rowid, text) VALUES ('delete', old.id, old.text);
    INSERT INTO chunks_fts(rowid, text) VALUES (new.id, new.text);
END;
)SQL";

} // namespace scribatic::db
