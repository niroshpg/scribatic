// =============================================================================
//  TranscriptFormat.hpp — the text a transcript becomes outside the app.
//
//  One implementation for both platforms: the share sheet on iOS and the
//  send intent on Android hand over byte-identical text, and the chunks the
//  search index is built from use the same speaker naming as what the user
//  reads. Internal — the engine exposes the results, not these functions.
// =============================================================================
#pragma once

#include "scribatic/core/Types.hpp"

#include <string>
#include <vector>

namespace scribatic::core {

/// "Speaker 3" for index 2. The one place the default label is spelled.
[[nodiscard]] std::string defaultSpeakerName(std::int32_t index);

/// Fills `displayName` from `name`, falling back to the default label.
void resolveDisplayNames(std::vector<SpeakerLabel>& speakers);

struct ExportOptions {
    bool includeTimestamps = true;
    bool anonymiseSpeakers = false;
};

/// Plain text: a title line, a one-line summary (date, length, speakers), then
/// one paragraph per turn. Consecutive segments from the same speaker merge
/// into one paragraph, including two labels the user gave the same name — the
/// intended way to repair a person the diarizer split in two.
[[nodiscard]] std::string formatTranscript(const NoteDetail& note, const ExportOptions& options);

/// One passage of a note for the search index, and where it is in the recording.
struct PassageText {
    std::string  text;
    std::int64_t startMs = 0;
    std::int64_t endMs   = 0;
};

/// The note as passages for the word and meaning indexes: "Name: words" per
/// turn, packed into runs of roughly `targetChars`. Speaker names are in the
/// text so "what did Sam say about the excursion" can match on both, and each
/// passage names its speakers even where a turn continues from the last one.
/// Built from segments, so a long turn is split instead of becoming a passage
/// too long to embed (MiniLM reads about 1000 characters).
[[nodiscard]] std::vector<PassageText> buildPassages(const NoteDetail& note,
                                                     std::size_t targetChars = 700);

} // namespace scribatic::core
