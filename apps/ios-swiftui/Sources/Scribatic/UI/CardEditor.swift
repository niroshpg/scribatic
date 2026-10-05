import SwiftUI

/// Correcting one card's text: names, numbers, a misheard word. A small
/// editor on purpose, beside the card's audio, not a document editor: the
/// recording and its times stay as they were, and what recognition wrote can
/// always be put back.
struct CardEditor: View {
    let block: TranscriptBlock
    let canPlay: Bool
    let playing: Bool
    let onPlay: () -> Void
    let onSave: (String) -> Void
    let onRestore: () -> Void
    let onCancel: () -> Void

    @State private var text: String
    @FocusState private var focused: Bool

    init(block: TranscriptBlock, canPlay: Bool, playing: Bool, onPlay: @escaping () -> Void,
         onSave: @escaping (String) -> Void, onRestore: @escaping () -> Void, onCancel: @escaping () -> Void) {
        self.block = block
        self.canPlay = canPlay
        self.playing = playing
        self.onPlay = onPlay
        self.onSave = onSave
        self.onRestore = onRestore
        self.onCancel = onCancel
        _text = State(initialValue: block.text)
    }

    private var trimmed: String { text.trimmingCharacters(in: .whitespacesAndNewlines) }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextEditor(text: $text)
                        .font(.uiBody)
                        .frame(minHeight: 160)
                        .focused($focused)
                } header: {
                    HStack {
                        Text(timestamp(block.startMs))
                        Spacer()
                        if canPlay {
                            Button(playing ? "Stop" : "Play this part", systemImage: playing ? "stop.fill" : "play.fill",
                                   action: onPlay)
                                .font(.uiLabel)
                                .textCase(nil)
                        }
                    }
                } footer: {
                    Text("Only the words change. The recording and its timing stay as they were.")
                }

                if block.edited {
                    Section {
                        Text(block.original)
                            .font(.uiBodySmall)
                            .foregroundStyle(Color.inkMuted)
                        Button("Restore original", systemImage: "arrow.uturn.backward", role: .destructive, action: onRestore)
                    } header: {
                        Text("As recognised")
                    }
                }
            }
            .navigationTitle("Edit text")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel", action: onCancel) }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") { onSave(trimmed) }
                        .disabled(trimmed.isEmpty || trimmed == block.text)
                }
            }
            .onAppear { focused = true }
        }
        .presentationDetents([.medium, .large])
        .interactiveDismissDisabled(trimmed != block.text)
    }
}
