#include "SpeakerAttribution.hpp"

#include <algorithm>
#include <limits>
#include <unordered_map>

namespace scribatic::core {
namespace {

/// A lone word labelled differently from both neighbours, when those two
/// agree, is almost always diarization jitter at a turn boundary rather than
/// someone getting a word in edgeways. Anything longer than this is left alone.
constexpr std::int64_t kMaxIslandMs = 800;

/// How much of a word's stored span counts when matching it to a turn. Word
/// timings run from one onset to the next, so the last word before a pause
/// stretches across the whole pause — and a turn-final word would be handed
/// to whoever speaks after the silence. No spoken word is this long; the
/// onset is where the evidence is.
constexpr std::int64_t kMaxWordSpanMs = 700;

std::int64_t overlap(std::int64_t a0, std::int64_t a1, std::int64_t b0, std::int64_t b1) {
    return std::max<std::int64_t>(0, std::min(a1, b1) - std::max(a0, b0));
}

std::int64_t gap(std::int64_t a0, std::int64_t a1, std::int64_t b0, std::int64_t b1) {
    if (a1 < b0) { return b0 - a1; }
    if (b1 < a0) { return a0 - b1; }
    return 0;
}

/// The speaker whose turns overlap [start, end] the most. With no overlap at
/// all — the word fell in a gap the diarizer called silence — the nearest turn
/// wins, because a word whisper heard was said by somebody.
std::int32_t speakerFor(std::int64_t start, std::int64_t end,
                        const std::vector<SpeakerTurn>& turns) {
    std::unordered_map<std::int32_t, std::int64_t> covered;
    std::int32_t nearest = -1;
    std::int64_t nearestGap = std::numeric_limits<std::int64_t>::max();

    for (const auto& turn : turns) {
        const std::int64_t o = overlap(start, end, turn.startMs, turn.endMs);
        if (o > 0) { covered[turn.speaker] += o; }
        const std::int64_t g = gap(start, end, turn.startMs, turn.endMs);
        if (g < nearestGap) {
            nearestGap = g;
            nearest = turn.speaker;
        }
    }

    std::int32_t best = -1;
    std::int64_t bestCovered = 0;
    for (const auto& [speaker, amount] : covered) {
        // Ties go to the lower id so the result does not depend on hash order.
        if (amount > bestCovered || (amount == bestCovered && speaker < best)) {
            best = speaker;
            bestCovered = amount;
        }
    }
    return best >= 0 ? best : nearest;
}

std::string trimmed(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\n");
    if (first == std::string::npos) { return {}; }
    const auto last = text.find_last_not_of(" \t\n");
    return text.substr(first, last - first + 1);
}

} // namespace

Attribution attributeSpeakers(const std::vector<WordTiming>& words,
                              const std::vector<SpeakerTurn>& turns) {
    Attribution result;
    if (words.empty()) { return result; }

    std::vector<std::int32_t> labels(words.size(), -1);
    if (!turns.empty()) {
        for (std::size_t i = 0; i < words.size(); ++i) {
            const std::int64_t end = std::min(words[i].endMs, words[i].startMs + kMaxWordSpanMs);
            labels[i] = speakerFor(words[i].startMs, std::max(end, words[i].startMs + 1), turns);
        }
        for (std::size_t i = 1; i + 1 < words.size(); ++i) {
            const bool island = labels[i] != labels[i - 1] && labels[i - 1] == labels[i + 1];
            if (island && words[i].endMs - words[i].startMs <= kMaxIslandMs) {
                labels[i] = labels[i - 1];
            }
        }
    }

    // Dense, first-appearance numbering. The diarizer's ids are cluster
    // indices: they can start anywhere and skip values.
    std::unordered_map<std::int32_t, std::int32_t> dense;
    for (auto& label : labels) {
        if (label < 0) { continue; }
        auto [it, inserted] = dense.emplace(label, static_cast<std::int32_t>(dense.size()));
        label = it->second;
    }
    result.speakerCount = static_cast<std::int32_t>(dense.size());

    std::string  text;
    float        probabilitySum = 0.0F;
    std::size_t  wordCount = 0;
    std::size_t  first = 0;

    const auto emit = [&](std::size_t last) {
        TranscriptSegment segment;
        segment.startMs    = words[first].startMs;
        segment.endMs      = std::max(words[last].endMs, segment.startMs);
        segment.text       = trimmed(text);
        segment.confidence = wordCount > 0 ? probabilitySum / static_cast<float>(wordCount) : 0.0F;
        segment.isFinal    = true;
        segment.speaker    = labels[first];
        if (!segment.text.empty()) { result.segments.push_back(std::move(segment)); }
        text.clear();
        probabilitySum = 0.0F;
        wordCount = 0;
    };

    for (std::size_t i = 0; i < words.size(); ++i) {
        if (i > first &&
            (labels[i] != labels[first] || words[i].segment != words[first].segment)) {
            emit(i - 1);
            first = i;
        }
        text += words[i].text;
        probabilitySum += words[i].probability;
        ++wordCount;
    }
    emit(words.size() - 1);

    return result;
}

} // namespace scribatic::core
