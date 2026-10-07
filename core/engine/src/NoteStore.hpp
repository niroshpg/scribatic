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
                            const std::vector<WordTiming>& words,
                            const std::string& language = "en");

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

    /// The accurate pass's result: segments and words replace the preview's,
    /// speakers are cleared (they are identified from the new words) and the
    /// note is marked refined. A non-empty `language` becomes the note's: the
    /// one detected for a recovered recording, which had none.
    EngineStatus replaceWithRefined(std::int64_t noteId,
                                    const std::vector<TranscriptSegment>& segments,
                                    const std::vector<WordTiming>& words,
                                    const std::string& language = {});

    /// Gives one segment to `speaker`, or to a new speaker when it is
    /// negative; `outSpeaker` gets the speaker it ended up with. Speakers
    /// left with nothing are removed and the rest renumbered 0..n-1.
    EngineStatus setSegmentSpeaker(std::int64_t noteId, std::int64_t segmentId,
                                   std::int32_t speaker, std::int32_t* outSpeaker);

    /// Every segment of `speakers` goes to `into`, which keeps its name.
    EngineStatus mergeSpeakers(std::int64_t noteId, const std::vector<std::int32_t>& speakers,
                               std::int32_t into);

    EngineStatus setLayout(std::int64_t noteId, const std::string& layout);

    /// A card's corrected text: all of it to the first of `segmentIds`, the
    /// rest emptied, so the card keeps its times and audio. Each segment's
    /// recognised text is kept the first time, for restoreText().
    EngineStatus editText(std::int64_t noteId, const std::vector<std::int64_t>& segmentIds,
                          const std::string& text);

    /// Puts the recognised text back on whichever of `segmentIds` were edited.
    EngineStatus restoreText(std::int64_t noteId, const std::vector<std::int64_t>& segmentIds);

    /// A consistent copy of the whole database at `path`, which must not
    /// exist yet: every note, transcript, correction and speaker, for a backup.
    EngineStatus snapshot(const std::string& path);

    struct RestoredNote {
        std::int64_t id = 0;
        std::string  audioName;   ///< the recording's name in the backup; empty if none
    };

    /// Copies in the notes of the database at `path` that this one does not
    /// have; same title, start and length counts as having it. The backup is
    /// first brought to the current schema, so an older app's backup restores.
    /// Recordings are the caller's: each new note comes back with its
    /// recording's name in the backup, and has none until setAudio().
    EngineStatus restoreFrom(const std::string& path, std::vector<RestoredNote>* restored,
                             std::int32_t* skipped);

    EngineStatus setAudio(std::int64_t noteId, const std::string& audioName);

    /// Every note's passages, or only those with no embedding yet.
    [[nodiscard]] std::vector<Passage> passages(bool withoutEmbeddingOnly);
    EngineStatus setPassageEmbedding(std::int64_t passageId, const Embedding& embedding);

    /// Passages containing the query's words, best first (FTS5, bm25).
    [[nodiscard]] std::vector<RetrievalHit> searchWords(const std::string& query, std::int32_t topK);

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
    EngineStatus compactSpeakers(std::int64_t noteId); // caller holds the transaction
    NoteDetail   loadLocked(std::int64_t noteId, const std::string& recordingsDirectory);

    std::mutex mutex_;
    sqlite3*   db_ = nullptr;
};

} // namespace scribatic::core
