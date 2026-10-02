// =============================================================================
//  SpeakerAttribution.hpp — joins "who spoke when" to "what was said".
//
//  Diarization and transcription are separate models with separate clocks.
//  Diarization produces turns (speaker k from 3.2 s to 7.9 s); whisper produces
//  segments that routinely run straight across a change of speaker, because it
//  segments on pauses and punctuation, not on voices. Labelling whole whisper
//  segments would therefore put one person's reply in the other's mouth.
//
//  Attribution works at the word level instead: each word takes the speaker
//  whose turn it overlaps most, and segments are re-cut wherever the speaker
//  changes. Internal — nothing here crosses the bridge.
// =============================================================================
#pragma once

#include "scribatic/core/Types.hpp"

#include <cstdint>
#include <unordered_map>
#include <string>
#include <vector>

namespace scribatic::core {

/// One word as whisper timed it. `text` keeps whisper's own leading space, so
/// concatenating words reproduces the segment text exactly.
struct WordTiming {
    std::int32_t segment  = 0;   ///< whisper segment the word came from
    std::int64_t startMs  = 0;
    std::int64_t endMs    = 0;
    std::string  text;
    float        probability = 0.0F;
};

/// One stretch of a single voice, as the diarizer reports it.
struct SpeakerTurn {
    std::int64_t startMs = 0;
    std::int64_t endMs   = 0;
    std::int32_t speaker = 0;    ///< diarizer's cluster id; not necessarily dense
};

struct Attribution {
    std::vector<TranscriptSegment> segments;
    std::int32_t                   speakerCount = 0;
    /// Diarizer cluster id -> the speaker number segments carry.
    std::unordered_map<std::int32_t, std::int32_t> speakerOfCluster;
};

/// Re-cuts `words` into segments that each belong to one speaker.
///
/// Speakers are renumbered by first appearance, so "Speaker 1" is whoever
/// spoke first — the diarizer's cluster ids are arbitrary and can skip
/// numbers. A segment boundary is kept wherever whisper had one, and added
/// wherever the speaker changes. `turns` may be empty, in which case every
/// segment is unattributed (speaker -1) and speakerCount is 0.
[[nodiscard]] Attribution attributeSpeakers(const std::vector<WordTiming>& words,
                                            const std::vector<SpeakerTurn>& turns);

} // namespace scribatic::core
