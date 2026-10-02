import SwiftUI

/// Consecutive segments shown as one card: one speaker's turn, or one
/// paragraph of a lecture.
struct TranscriptBlock: Identifiable {
    let speaker: Int32
    var segments: [TranscriptSegmentValue]

    var id: Int64 { segments[0].segmentId != 0 ? segments[0].segmentId : segments[0].startMs }
    var startMs: Int64 { segments[0].startMs }
    var segmentIds: [Int64] { segments.map(\.segmentId) }
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

/// One speaker's turn: their avatar beside a card, in the manner of a chat.
/// The title is the name the user gave; until then only the avatar says who
/// it is. Tapping either opens `onSpeaker`, to rename, move or split it off.
struct DiscussionCard: View {
    let block: TranscriptBlock
    let label: SpeakerLabelValue?
    let onSpeaker: () -> Void

    var body: some View {
        let identified = block.speaker >= 0
        let name = label?.name.trimmingCharacters(in: .whitespaces) ?? ""
        HStack(alignment: .top, spacing: 10) {
            if identified {
                Button(action: onSpeaker) {
                    SpeakerAvatar(initials: speakerInitials(label, index: block.speaker), index: block.speaker)
                }
                .buttonStyle(.plain)
                .accessibilityLabel("\(label?.displayName ?? "Speaker"), change speaker")
            }
            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    if identified && !name.isEmpty {
                        Button(action: onSpeaker) {
                            Text(name)
                                .font(.uiLabel.weight(.semibold))
                                .foregroundStyle(Color.speaker(block.speaker))
                        }
                        .buttonStyle(.plain)
                    }
                    Spacer()
                    Text(timestamp(block.startMs))
                        .font(.timestamp)
                        .foregroundStyle(Color.inkSoft)
                }
                ForEach(block.segments) { segment in
                    Text(segment.text)
                        .font(.uiBody)
                        .foregroundStyle(Color.ink)
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 10)
            .background(Color.surfaceRaised,
                        in: UnevenRoundedRectangle(topLeadingRadius: 4, bottomLeadingRadius: 16,
                                                   bottomTrailingRadius: 16, topTrailingRadius: 16))
        }
    }
}

/// A paragraph of a lecture: no avatar and no title, one voice throughout.
struct LectureCard: View {
    let block: TranscriptBlock

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(timestamp(block.startMs))
                .font(.timestamp)
                .foregroundStyle(Color.inkSoft)
            Text(block.segments.map(\.text).joined(separator: " "))
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
