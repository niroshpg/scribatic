// =============================================================================
//  Queries.hpp — Prepared-statement text shared by both platforms.
//  Every statement is parameterised; no string interpolation anywhere in the
//  codebase touches SQL.
// =============================================================================
#pragma once

#include <string_view>

namespace scribatic::db::queries {

/// `audio_path` is a file name relative to the recordings directory — see
/// EngineConfig::recordingsDirectory for why it is never absolute.
inline constexpr std::string_view kInsertNote =
    "INSERT INTO notes(title, created_at, duration_ms, locale, audio_path) "
    "VALUES(?, ?, ?, ?, ?);";

inline constexpr std::string_view kInsertSegment =
    "INSERT INTO segments(note_id, start_ms, end_ms, text, confidence, speaker) "
    "VALUES(?, ?, ?, ?, ?, ?);";

inline constexpr std::string_view kInsertWord =
    "INSERT INTO words(note_id, ordinal, segment, start_ms, end_ms, text, probability) "
    "VALUES(?, ?, ?, ?, ?, ?, ?);";

inline constexpr std::string_view kInsertSpeaker =
    "INSERT INTO speakers(note_id, idx, name) VALUES(?, ?, '');";

/// Newest first, with the opening of the transcript as a preview.
inline constexpr std::string_view kListNotes =
    "SELECT n.id, n.title, n.created_at, n.duration_ms, n.audio_path IS NOT NULL, "
    "       n.speaker_count, "
    "       (SELECT group_concat(text, ' ') FROM "
    "           (SELECT text FROM segments WHERE note_id = n.id "
    "            ORDER BY start_ms, id LIMIT 3)) "
    "FROM notes n ORDER BY n.created_at DESC, n.id DESC;";

inline constexpr std::string_view kLoadNote =
    "SELECT id, title, created_at, duration_ms, audio_path, speaker_count, summary, locale, "
    "       layout, refined "
    "FROM notes WHERE id = ?;";

inline constexpr std::string_view kSetLayout = "UPDATE notes SET layout = ? WHERE id = ?;";
inline constexpr std::string_view kSetRefined = "UPDATE notes SET refined = ? WHERE id = ?;";
inline constexpr std::string_view kDeleteWords = "DELETE FROM words WHERE note_id = ?;";

inline constexpr std::string_view kSetSegmentSpeaker =
    "UPDATE segments SET speaker = ? WHERE id = ? AND note_id = ?;";
inline constexpr std::string_view kMoveSpeaker =
    "UPDATE segments SET speaker = ? WHERE note_id = ? AND speaker = ?;";
inline constexpr std::string_view kDeleteSpeaker =
    "DELETE FROM speakers WHERE note_id = ? AND idx = ?;";
inline constexpr std::string_view kMaxSpeaker =
    "SELECT COALESCE(MAX(idx), -1) FROM speakers WHERE note_id = ?;";
inline constexpr std::string_view kCountSpeakers =
    "SELECT COUNT(*) FROM speakers WHERE note_id = ?;";

inline constexpr std::string_view kSetSummary =
    "UPDATE notes SET summary = ? WHERE id = ?;";

inline constexpr std::string_view kLoadSegments =
    "SELECT start_ms, end_ms, text, confidence, speaker, id FROM segments "
    "WHERE note_id = ? ORDER BY start_ms, id;";

inline constexpr std::string_view kLoadSpeakers =
    "SELECT idx, name FROM speakers WHERE note_id = ? ORDER BY idx;";

inline constexpr std::string_view kLoadWords =
    "SELECT segment, start_ms, end_ms, text, probability FROM words "
    "WHERE note_id = ? ORDER BY ordinal;";

inline constexpr std::string_view kDeleteSegments = "DELETE FROM segments WHERE note_id = ?;";
inline constexpr std::string_view kDeleteSpeakers = "DELETE FROM speakers WHERE note_id = ?;";
inline constexpr std::string_view kDeleteChunks   = "DELETE FROM chunks WHERE note_id = ?;";

inline constexpr std::string_view kUpdateSpeakerCount =
    "UPDATE notes SET speaker_count = ? WHERE id = ?;";

inline constexpr std::string_view kRenameSpeaker =
    "UPDATE speakers SET name = ? WHERE note_id = ? AND idx = ?;";

inline constexpr std::string_view kNoteAudio = "SELECT audio_path FROM notes WHERE id = ?;";

inline constexpr std::string_view kClearAudio =
    "UPDATE notes SET audio_path = NULL WHERE id = ?;";

inline constexpr std::string_view kReferencedAudio =
    "SELECT id, audio_path FROM notes WHERE audio_path IS NOT NULL;";

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
