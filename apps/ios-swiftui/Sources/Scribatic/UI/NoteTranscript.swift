import SwiftUI

/// Consecutive segments shown as one card: one speaker's turn, or one
/// paragraph of a lecture.
struct TranscriptBlock: Identifiable {
    let speaker: Int32
    var segments: [TranscriptSegmentValue]

    var id: Int64 { segments[0].segmentId != 0 ? segments[0].segmentId : segments[0].startMs }
    var startMs: Int64 { segments[0].startMs }
    var endMs: Int64 { segments[segments.count - 1].endMs }
    var segmentIds: [Int64] { segments.map(\.segmentId) }
    /// What the card says. A corrected card keeps it all in its first
    /// segment; the others are empty.
    var text: String { segments.map(\.text).filter { !$0.isEmpty }.joined(separator: " ") }
    var edited: Bool { segments.contains(where: \.edited) }
    /// What recognition wrote for the card, before any correction.
    var original: String {
        segments.map { $0.edited ? $0.original : $0.text }.filter { !$0.isEmpty }.joined(separator: " ")
    }
}

/// A card's controls that sit beside its time: play just this part, and a
/// mark once its text has been corrected.
struct CardTimeRow: View {
    let block: TranscriptBlock
    let playing: Bool
    let onPlay: (() -> Void)?

    var body: some View {
        HStack(spacing: 8) {
            if let onPlay {
                Button(action: onPlay) {
                    Image(systemName: playing ? "stop.fill" : "play.fill")
                        .font(.system(size: 10, weight: .bold))
                        .foregroundStyle(Color.inkMuted)
                        .frame(width: 22, height: 22)
                        .background(Color.paper, in: Circle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel(playing ? "Stop" : "Play this part")
            }
            Text(timestamp(block.startMs))
                .font(.timestamp)
                .foregroundStyle(Color.inkSoft)
            if block.edited {
                Text("Edited")
                    .font(.uiCaption)
                    .foregroundStyle(Color.inkSoft)
                    .accessibilityLabel("Text corrected")
            }
        }
    }
}

/// A discussion: a new card wherever the speaker changes.
func discussionBlocks(_ segments: [TranscriptSegmentValue]) -> [TranscriptBlock] {
    var blocks: [TranscriptBlock] = []
    for segment in segments {
        if let last = blocks.indices.last, blocks[last].speaker == segment.speaker {
            blocks[last].segments.append(segment)
        } else {
            blocks.append(TranscriptBlock(speaker: segment.speaker, segments: [segment]))
        }
    }
    return blocks
}

/// A lecture: one voice, so cards are paragraphs — a new one after a pause,
/// or once a paragraph runs long enough to be hard to read as one.
func lectureBlocks(_ segments: [TranscriptSegmentValue], pauseMs: Int64 = 2500, maxPerBlock: Int = 6) -> [TranscriptBlock] {
    var blocks: [TranscriptBlock] = []
    for segment in segments {
        if let last = blocks.indices.last,
           blocks[last].segments.count < maxPerBlock,
           let previous = blocks[last].segments.last,
           segment.startMs - previous.endMs < pauseMs {
            blocks[last].segments.append(segment)
        } else {
            blocks.append(TranscriptBlock(speaker: segment.speaker, segments: [segment]))
        }
    }
    return blocks
}

/// "S1" until the speaker is named, then their initials: "AN" for "Ana",
/// "PS" for "Priya Shah".
func speakerInitials(_ label: SpeakerLabelValue?, index: Int32) -> String {
    let name = label?.name.trimmingCharacters(in: .whitespaces) ?? ""
    guard !name.isEmpty else { return "S\(index + 1)" }
    let words = name.split(whereSeparator: \.isWhitespace)
    if words.count >= 2, let a = words[0].first, let b = words[1].first {
        return "\(a)\(b)".uppercased()
    }
    return String(name.prefix(2)).uppercased()
}

struct SpeakerAvatar: View {
    let initials: String
    let index: Int32
    var size: CGFloat = 36

    var body: some View {
        Text(initials)
            .font(.uiLabel.weight(.semibold))
            .foregroundStyle(Color.speaker(index))
            .frame(width: size, height: size)
            .background(Color.speaker(index).opacity(0.18), in: Circle())
            .accessibilityHidden(true)
    }
}

/// One speaker's turn: a card the full width of the screen, its avatar and
/// name in the header beside the time, so the text has the whole width.
/// The title is the name the user gave; until then only the avatar says who
/// it is. Tapping either opens `onSpeaker`, to rename, move or split it off.
struct DiscussionCard: View {
    let block: TranscriptBlock
    let label: SpeakerLabelValue?
    var playing = false
    var onPlay: (() -> Void)?
    let onSpeaker: () -> Void

    var body: some View {
        let identified = block.speaker >= 0
        let name = label?.name.trimmingCharacters(in: .whitespaces) ?? ""
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                if identified {
                    Button(action: onSpeaker) {
                        HStack(spacing: 8) {
                            SpeakerAvatar(initials: speakerInitials(label, index: block.speaker),
                                          index: block.speaker, size: 28)
                            if !name.isEmpty {
                                Text(name)
                                    .font(.uiLabel.weight(.semibold))
                                    .foregroundStyle(Color.speaker(block.speaker))
                            }
                        }
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel("\(label?.displayName ?? "Speaker"), change speaker")
                }
                Spacer()
                CardTimeRow(block: block, playing: playing, onPlay: onPlay)
            }
            ForEach(block.segments.filter { !$0.text.isEmpty }) { segment in
                Text(segment.text)
                    .font(.uiBody)
                    .foregroundStyle(Color.ink)
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .background(Color.surfaceRaised, in: RoundedRectangle(cornerRadius: 16))
    }
}

/// A paragraph of a lecture: no avatar and no title, one voice throughout.
struct LectureCard: View {
    let block: TranscriptBlock
    var playing = false
    var onPlay: (() -> Void)?

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            CardTimeRow(block: block, playing: playing, onPlay: onPlay)
            Text(block.text)
                .font(.uiBody)
                .foregroundStyle(Color.ink)
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .background(Color.surfaceRaised, in: RoundedRectangle(cornerRadius: 16))
    }
}
