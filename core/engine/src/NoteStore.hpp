// =============================================================================
//  NoteStore.hpp — the on-device SQLite store behind the notes API.
//
//  Owns the only sqlite3 handle in the process. Internal: `sqlite3` never
//  appears in a public header, for the same reason ggml does not.
//
//  THREADING
//  ---------
//  Every method takes one mutex. The iOS actor already serialises calls, but
//  Kotlin dispatches onto a pool, so two engine calls can arrive concurrently
//  and a sqlite3 connection must not be used from two threads at once.
// =============================================================================
#pragma once

#include "SpeakerAttribution.hpp"
#include "scribatic/core/Types.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace scribatic::core {

class NoteStore {
public:
    NoteStore() = default;
    ~NoteStore();
    NoteStore(const NoteStore&) = delete;
    NoteStore& operator=(const NoteStore&) = delete;

    /// Opens (creating if needed) and brings the schema to kSchemaVersion.
    EngineStatus open(const std::string& path);
    void         close();
    [[nodiscard]] bool isOpen();

    /// Inserts a note with its segments and word timings in one transaction.
    /// Returns the new id, or 0 on failure.
    std::int64_t insertNote(const std::string& title, std::int64_t createdAt,
                            std::int64_t durationMs, const std::string& audioName,
                            const std::vector<TranscriptSegment>& segments,
                            const std::vector<WordTiming>& words);

    [[nodiscard]] std::vector<NoteSummary> list();

    /// `audioPath` in the result is resolved against `recordingsDirectory`.
    [[nodiscard]] NoteDetail load(std::int64_t noteId, const std::string& recordingsDirectory);

    [[nodiscard]] std::vector<WordTiming> words(std::int64_t noteId);

    /// Replaces the note's segments and speakers (names reset) and rebuilds
    /// its search chunks.
    EngineStatus replaceTranscript(std::int64_t noteId,
                                   const std::vector<TranscriptSegment>& segments,
                                   std::int32_t speakerCount);

    EngineStatus renameSpeaker(std::int64_t noteId, std::int32_t speaker,
                               const std::string& name);

    /// Empty stores NULL: the note has no summary.
    EngineStatus setSummary(std::int64_t noteId, const std::string& summary);

    /// File name of the note's recording; empty if it has none.
    [[nodiscard]] std::string audioName(std::int64_t noteId);
    EngineStatus clearAudio(std::int64_t noteId);

    /// Deletes the note row; the cascade and the FTS triggers take the rest.
    /// Writes the recording's file name, if any, to `audioName` so the caller
    /// can remove the file once the delete has committed.
    EngineStatus deleteNote(std::int64_t noteId, std::string* audioName);

    struct AudioReference {
        std::int64_t noteId = 0;
        std::string  audioName;
    };
    [[nodiscard]] std::vector<AudioReference> audioReferences();

private:
    EngineStatus migrate();
    EngineStatus rebuildChunks(std::int64_t noteId);   // caller holds the transaction
    NoteDetail   loadLocked(std::int64_t noteId, const std::string& recordingsDirectory);

    std::mutex mutex_;
    sqlite3*   db_ = nullptr;
};

} // namespace scribatic::core
