// =============================================================================
//  Queries.hpp — Prepared-statement text shared by both platforms.
//  Every statement is parameterised; no string interpolation anywhere in the
//  codebase touches SQL.
// =============================================================================
#pragma once

#include <string_view>

namespace scribatic::db::queries {

inline constexpr std::string_view kInsertNote =
    "INSERT INTO notes(title, created_at, duration_ms, locale) VALUES(?, ?, ?, ?);";

inline constexpr std::string_view kInsertSegment =
    "INSERT INTO segments(note_id, start_ms, end_ms, text, confidence) "
    "VALUES(?, ?, ?, ?, ?);";

inline constexpr std::string_view kInsertChunk =
    "INSERT INTO chunks(note_id, ordinal, text) VALUES(?, ?, ?);";

/// Vector upsert — rowid is bound to the owning chunks.id.
inline constexpr std::string_view kUpsertEmbedding =
    "INSERT OR REPLACE INTO vss_chunks(rowid, embedding) VALUES(?, ?);";

/// Approximate nearest neighbour. `?1` is the packed float32 query vector.
inline constexpr std::string_view kVectorSearch =
    "SELECT c.note_id, c.id, c.text, v.distance "
    "FROM vss_chunks v "
    "JOIN chunks c ON c.id = v.rowid "
    "WHERE vss_search(v.embedding, ?1) "
    "LIMIT ?2;";

/// Lexical arm of the hybrid retriever.
inline constexpr std::string_view kLexicalSearch =
    "SELECT c.note_id, c.id, c.text, bm25(chunks_fts) AS score "
    "FROM chunks_fts f JOIN chunks c ON c.id = f.rowid "
    "WHERE chunks_fts MATCH ?1 ORDER BY score LIMIT ?2;";

inline constexpr std::string_view kDeleteNoteCascade =
    "DELETE FROM notes WHERE id = ?;";

} // namespace scribatic::db::queries
