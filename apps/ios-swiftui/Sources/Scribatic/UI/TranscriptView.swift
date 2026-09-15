import SwiftUI

extension Color {
    /// Atomic tangerine — the same accent the diagrams, the docs and
    /// scribatic.com use, so the product reads as one thing.
    static let scribaticAccent = Color(red: 235 / 255, green: 108 / 255, blue: 54 / 255)
}

struct TranscriptView: View {
    @Bindable var model: TranscriptionModel

    var body: some View {
        NavigationStack {
            content
                .navigationTitle("Transcript")
                // The content is a column of prose. Left to itself a List spans
                // the full width of an iPad, which gives very long measure and
                // an empty-looking sheet; capping it keeps the line length
                // readable and the layout deliberate rather than stretched.
                .frame(maxWidth: 720)
                .frame(maxWidth: .infinity)
                .safeAreaInset(edge: .bottom) { statusBar }
                .task { await model.start() }
        }
    }

    @ViewBuilder
    private var content: some View {
        if let failure = model.failureMessage {
            ContentUnavailableView {
                Label("Engine stopped", systemImage: "exclamationmark.triangle")
            } description: {
                Text(failure)
            } actions: {
                Button("Try again") {
                    Task { await model.start() }
                }
                .buttonStyle(.borderedProminent)
                .tint(.scribaticAccent)
            }
        } else if model.segments.isEmpty {
            ContentUnavailableView {
                Label("Nothing transcribed yet", systemImage: "waveform")
            } description: {
                Text("Speech picked up by the microphone is transcribed on this device and appears here. Nothing is uploaded.")
            }
        } else {
            List(model.segments) { segment in
                VStack(alignment: .leading, spacing: 4) {
                    Text(segment.text)
                        .font(.body)
                        // An interim segment can still change; dimming it says
                        // so without needing a label to explain it.
                        .foregroundStyle(segment.isFinal ? .primary : .secondary)

                    Text(timestamp(segment))
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.tertiary)
                }
                .padding(.vertical, 2)
            }
            .listStyle(.plain)
        }
    }

    /// The status was a `.caption` in a toolbar status slot: a few grey pixels
    /// that read as one small word on a phone and vanished on an iPad. It is
    /// now a legible bar pinned to the bottom edge, with a colour that carries
    /// the state on its own.
    private var statusBar: some View {
        HStack(spacing: 10) {
            Circle()
                .fill(statusColor)
                .frame(width: 10, height: 10)

            Text(model.phase.label)
                .font(.subheadline.weight(.medium))
                .foregroundStyle(model.phase.isFailure ? Color.primary : .secondary)

            Spacer()

            if !model.segments.isEmpty {
                Text("^[\(model.segments.count) segment](inflect: true)")
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.tertiary)
            }
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 12)
        .frame(maxWidth: 720)
        .frame(maxWidth: .infinity)
        .background(.bar)
        .overlay(alignment: .top) { Divider() }
        .accessibilityElement(children: .combine)
        .accessibilityLabel("Engine status: \(model.phase.label)")
    }

    private var statusColor: Color {
        switch model.phase {
        case .idle:                    return .secondary
        case .starting:                return .yellow
        case .listening, .transcribing: return .scribaticAccent
        case .failed:                  return .red
        }
    }

    private func timestamp(_ segment: TranscriptSegmentValue) -> String {
        let start = Duration.milliseconds(segment.startMs)
        return start.formatted(.time(pattern: .minuteSecond))
    }
}
