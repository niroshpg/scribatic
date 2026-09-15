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
                .toolbar {
                    ToolbarItem(placement: .topBarTrailing) {
                        Button("Clear", systemImage: "trash") { model.clear() }
                            .disabled(!model.canClear)
                    }
                }
                .safeAreaInset(edge: .bottom) { transportBar }
                .task { await model.prepare() }
        }
    }

    // MARK: - Body

    @ViewBuilder
    private var content: some View {
        if let failure = model.failureMessage {
            ContentUnavailableView {
                Label("Engine stopped", systemImage: "exclamationmark.triangle")
            } description: {
                Text(failure)
            } actions: {
                Button("Try again") {
                    Task { await model.prepare() }
                }
                .buttonStyle(.borderedProminent)
                .tint(.scribaticAccent)
            }
        } else if model.segments.isEmpty {
            ContentUnavailableView {
                Label("Nothing transcribed yet", systemImage: "waveform")
            } description: {
                Text("Press record to start. Speech is transcribed on this device and appears here — nothing is uploaded, and the microphone is only open while you are recording.")
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

    // MARK: - Transport

    /// Record / pause / resume / stop / play, plus the status. Which controls
    /// appear is driven entirely by the phase, so there is never a button that
    /// does nothing in the current state.
    private var transportBar: some View {
        VStack(spacing: 12) {
            Divider()

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

            HStack(spacing: 16) {
                switch model.phase {
                case .starting, .failed:
                    EmptyView()

                case .ready:
                    recordButton
                    if model.hasRecording {
                        transportButton("Play", systemImage: "play.fill") { model.play() }
                    }

                case .recording:
                    transportButton("Pause", systemImage: "pause.fill") { model.pauseRecording() }
                    stopButton

                case .paused:
                    transportButton("Resume", systemImage: "play.fill") { model.resumeRecording() }
                    stopButton

                case .playing:
                    transportButton("Stop", systemImage: "stop.fill") { model.stopPlayback() }
                }
            }
            .frame(maxWidth: .infinity)
        }
        .padding(.horizontal, 20)
        .padding(.bottom, 12)
        .frame(maxWidth: 720)
        .frame(maxWidth: .infinity)
        .background(.bar)
    }

    private var recordButton: some View {
        Button {
            Task { await model.startRecording() }
        } label: {
            Label("Record", systemImage: "mic.fill")
                .font(.headline)
                .padding(.horizontal, 8)
        }
        .buttonStyle(.borderedProminent)
        .controlSize(.large)
        .tint(.scribaticAccent)
        .accessibilityHint("Opens the microphone and begins transcribing on this device")
    }

    private var stopButton: some View {
        transportButton("Stop", systemImage: "stop.fill") { model.stopRecording() }
    }

    private func transportButton(
        _ title: String,
        systemImage: String,
        action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            Label(title, systemImage: systemImage)
                .font(.headline)
                .padding(.horizontal, 8)
        }
        .buttonStyle(.bordered)
        .controlSize(.large)
    }

    private var statusColor: Color {
        switch model.phase {
        case .starting:            return .yellow
        case .ready:               return .secondary
        case .recording:           return .scribaticAccent
        case .paused:              return .yellow
        case .playing:             return .blue
        case .failed:              return .red
        }
    }

    private func timestamp(_ segment: TranscriptSegmentValue) -> String {
        Duration.milliseconds(segment.startMs).formatted(.time(pattern: .minuteSecond))
    }
}
