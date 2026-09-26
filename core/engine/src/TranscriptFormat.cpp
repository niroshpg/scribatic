#include "TranscriptFormat.hpp"

#include <cstdio>
#include <ctime>

namespace scribatic::core {
namespace {

struct Paragraph {
    std::int64_t startMs = 0;
    std::string  speaker;        ///< empty when speakers were never identified
    std::string  text;
};

std::string speakerNameFor(const NoteDetail& note, std::int32_t index, bool anonymise) {
    if (index < 0) { return {}; }
    if (!anonymise) {
        for (const auto& speaker : note.speakers) {
            if (speaker.index == index) {
                return speaker.displayName.empty() ? defaultSpeakerName(index)
                                                   : speaker.displayName;
            }
        }
    }
    return defaultSpeakerName(index);
}

std::vector<Paragraph> paragraphs(const NoteDetail& note, bool anonymise) {
    std::vector<Paragraph> out;
    for (const auto& segment : note.segments) {
        std::string speaker = speakerNameFor(note, segment.speaker, anonymise);
        if (!out.empty() && out.back().speaker == speaker) {
            out.back().text += ' ';
            out.back().text += segment.text;
            continue;
        }
        out.push_back({segment.startMs, std::move(speaker), segment.text});
    }
    return out;
}

std::string clock(std::int64_t ms) {
    const std::int64_t seconds = ms / 1000;
    char buffer[32];
    if (seconds >= 3600) {
        std::snprintf(buffer, sizeof buffer, "%lld:%02lld:%02lld",
                      static_cast<long long>(seconds / 3600),
                      static_cast<long long>((seconds / 60) % 60),
                      static_cast<long long>(seconds % 60));
    } else {
        std::snprintf(buffer, sizeof buffer, "%02lld:%02lld",
                      static_cast<long long>(seconds / 60),
                      static_cast<long long>(seconds % 60));
    }
    return buffer;
}

std::string duration(std::int64_t ms) {
    const std::int64_t seconds = (ms + 500) / 1000;
    char buffer[32];
    if (seconds < 60) {
        std::snprintf(buffer, sizeof buffer, "%lld s", static_cast<long long>(seconds));
    } else {
        std::snprintf(buffer, sizeof buffer, "%lld min %lld s",
                      static_cast<long long>(seconds / 60),
                      static_cast<long long>(seconds % 60));
    }
    return buffer;
}

std::string date(std::int64_t epochSeconds) {
    const std::time_t time = static_cast<std::time_t>(epochSeconds);
    std::tm local {};
    if (::localtime_r(&time, &local) == nullptr) { return {}; }
    char buffer[32];
    std::strftime(buffer, sizeof buffer, "%Y-%m-%d %H:%M", &local);
    return buffer;
}

} // namespace

std::string defaultSpeakerName(std::int32_t index) {
    return "Speaker " + std::to_string(index + 1);
}

void resolveDisplayNames(std::vector<SpeakerLabel>& speakers) {
    for (auto& speaker : speakers) {
        speaker.displayName = speaker.name.empty() ? defaultSpeakerName(speaker.index)
                                                   : speaker.name;
    }
}

std::string formatTranscript(const NoteDetail& note, const ExportOptions& options) {
    std::string out = note.title;
    out += "\n";
    out += date(note.createdAt);
    out += " \xC2\xB7 ";                                  // U+00B7 middle dot
    out += duration(note.durationMs);
    if (note.speakerCount > 0) {
        out += " \xC2\xB7 ";
        out += std::to_string(note.speakerCount);
        out += note.speakerCount == 1 ? " speaker" : " speakers";
    }
    out += "\n";

    for (const auto& paragraph : paragraphs(note, options.anonymiseSpeakers)) {
        out += "\n";
        if (options.includeTimestamps) {
            out += "[" + clock(paragraph.startMs) + "] ";
        }
        if (!paragraph.speaker.empty()) {
            out += paragraph.speaker + ": ";
        }
        out += paragraph.text;
        out += "\n";
    }
    return out;
}

std::vector<std::string> buildChunks(const NoteDetail& note, std::size_t targetChars) {
    std::vector<std::string> chunks;
    std::string current;
    for (const auto& paragraph : paragraphs(note, /*anonymise=*/false)) {
        std::string line = paragraph.speaker.empty() ? paragraph.text
                                                     : paragraph.speaker + ": " + paragraph.text;
        if (!current.empty() && current.size() + line.size() > targetChars) {
            chunks.push_back(std::move(current));
            current.clear();
        }
        if (!current.empty()) { current += "\n"; }
        current += line;
    }
    if (!current.empty()) { chunks.push_back(std::move(current)); }
    return chunks;
}

} // namespace scribatic::core
