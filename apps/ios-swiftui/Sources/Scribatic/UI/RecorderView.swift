import SwiftUI

/// The live recording screen. Pushed over the notes list, and capture starts
/// as it appears, so one tap on the record button is enough. When the
/// recording is stopped and saved, the model replaces it with the note.
struct RecorderView: View {
    @Bindable var model: TranscriptionModel

    var body: some View {
        content
            .navigationTitle("New recording")
            .navigationBarTitleDisplayMode(.inline)
            // Leaving mid-recording would orphan the capture; the only way
            // out is Stop, which saves it.
            .navigationBarBackButtonHidden(isCapturing)
            // The content is a column of prose. Left to itself a List spans
            // the full width of an iPad, which gives very long measure and
            // an empty-looking sheet; capping it keeps the line length
            // readable and the layout deliberate rather than stretched.
            .frame(maxWidth: 720)
            .frame(maxWidth: .infinity)
            .background(Color.paper)
            .safeAreaInset(edge: .bottom) { transportBar }
            .sensoryFeedback(trigger: model.phase) { _, phase in
                switch phase {
                case .recording, .processing: .impact(weight: .medium)
                case .paused: .impact(weight: .light)
                default: nil
                }
            }
            .task {
                if model.phase == .ready { await model.startRecording() }
            }
    }

    private var isCapturing: Bool {
        switch model.phase {
        case .recording, .paused, .processing: return true
        default: return false
        }
    }

    // MARK: - Body

    @ViewBuilder
    private var content: some View {
        if let failure = model.failureMessage {
            EmptyStateView(title: "Engine stopped", message: failure, failure: true)
        } else if model.segments.isEmpty {
            EmptyStateView(
                title: model.phase == .ready ? "Ready to record" : "Listening",
                message: "Speech is transcribed on this device and appears here — nothing is uploaded, and the microphone is only open while you are recording. When you stop, Scribatic works out who said what."
            )
        } else {
            ScrollViewReader { proxy in
                List(model.segments) { segment in
                    VStack(alignment: .leading, spacing: 2) {
                        Text(timestamp(segment.startMs))
                            .font(.timestamp)
                            .foregroundStyle(Color.inkSoft)
                        Text(segment.text)
                            .font(.uiBody)
                            // An interim segment can still change; dimming it
                            // says so without needing a label to explain it.
                            .foregroundStyle(segment.isFinal ? Color.ink : Color.inkMuted)
                    }
                    .listRowInsets(EdgeInsets(top: 5, leading: 16, bottom: 5, trailing: 16))
                    .listRowBackground(Color.paper)
                    .listRowSeparator(.hidden)
                    .id(segment.id)
                }
                .listStyle(.plain)
                .environment(\.defaultMinListRowHeight, 0)
                .scrollContentBackground(.hidden)
                // Follow the words as they arrive; otherwise the newest
                // sentence lands below the fold after half a minute.
                .onChange(of: model.segments.count) {
                    if let last = model.segments.last {
                        withAnimation { proxy.scrollTo(last.id, anchor: .bottom) }
                    }
                }
            }
        }
    }

    // MARK: - Transport

    /// Status and controls, in thumb reach. Which controls appear is driven
    /// entirely by the phase, so there is never a button that does nothing in
    /// the current state.
    private var transportBar: some View {
        VStack(spacing: 16) {
            HStack {
                StatusPill(phase: model.phase)
                Spacer()
                if !model.segments.isEmpty {
                    Text(model.segments.count == 1 ? "1 line" : "\(model.segments.count) lines")
                        .font(.timestamp)
                        .foregroundStyle(Color.inkMuted)
                }
            }

            switch model.phase {
            case .starting, .failed, .processing:
                EmptyView()

            case .ready:
                LabelledControl(label: "Record") {
                    RecordButton(stop: false, label: "Record") {
                        Task { await model.startRecording() }
                    }
                    .accessibilityHint("Opens the microphone and begins transcribing on this device")
                }

            case .recording, .paused:
                // Bottom-aligned, so the labels under the two sizes of button share a line.
                HStack(alignment: .bottom) {
                    Group {
                        if model.phase == .recording {
                            LabelledControl(label: "Pause") {
                                RoundButton(systemImage: "pause.fill", label: "Pause") { model.pauseRecording() }
                            }
                        } else {
                            LabelledControl(label: "Resume") {
                                RoundButton(systemImage: "play.fill", label: "Resume") { model.resumeRecording() }
                            }
                        }
                    }
                    .frame(maxWidth: .infinity)
                    LabelledControl(label: "Stop") {
                        RecordButton(stop: true, label: "Stop and save") { model.stopRecording() }
                    }
                    .frame(maxWidth: .infinity)
                    Color.clear.frame(maxWidth: .infinity, maxHeight: 1)
                }
            }
        }
        .padding(.horizontal, 20)
        .padding(.top, 14)
        .padding(.bottom, 12)
        .frame(maxWidth: 720)
        .frame(maxWidth: .infinity)
        .background {
            // Down under the home indicator, so the panel reads as one surface.
            UnevenRoundedRectangle(topLeadingRadius: 20, topTrailingRadius: 20)
                .fill(Color.surfaceRaised)
                .overlay {
                    UnevenRoundedRectangle(topLeadingRadius: 20, topTrailingRadius: 20)
                        .stroke(Color.line, lineWidth: 1)
                }
                .ignoresSafeArea(edges: .bottom)
        }
    }
}
