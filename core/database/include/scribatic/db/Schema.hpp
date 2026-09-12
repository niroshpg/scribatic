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
inline constexpr std::int32_t kSchemaVersion   = 1;
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

/// Ordered bootstrap sequence executed inside a single transaction.
inline constexpr std::array<std::string_view, 5> kBootstrapStatements{
    kCreateNotes,
    kCreateSegments,
    kCreateChunks,
    kCreateVectorIndex,
    kCreateFts,
};

} // namespace scribatic::db
