import SwiftUI

/// A note's speakers, behind the "2 speakers" chip rather than in the way of
/// the transcript: name them, say how many people were talking, or merge
/// two labels that are one person. Who said a single part is changed from
/// that part's icon in the transcript.
struct SpeakersSheet: View {
    let note: NoteDetailValue
    /// The recording is here and the speaker models are installed.
    let canIdentify: Bool
    let onRename: (Int32, String) -> Void
    let onIdentify: (Int32) -> Void
    let onMerge: ([Int32], Int32) -> Void

    @Environment(\.dismiss) private var dismiss
    @State private var names: [Int32: String] = [:]
    @State private var choosingCount = false

    var body: some View {
        NavigationStack {
            List {
                Section {
                    ForEach(note.speakers) { speaker in
                        HStack(spacing: 12) {
                            SpeakerAvatar(initials: speakerInitials(speaker, index: speaker.index), index: speaker.index, size: 28)
                            TextField(speaker.displayName, text: name(speaker))
                                .textInputAutocapitalization(.words)
                                .submitLabel(.done)
                                .onSubmit { save(speaker) }
                        }
                        .listRowBackground(Color.surfaceRaised)
                    }
                } footer: {
                    Text("Names stay on this device, for this note only. Leave one empty for the numbered label.")
                }

                Section {
                    if canIdentify {
                        Button("Set how many people were talking…", systemImage: "person.2.wave.2") { choosingCount = true }
                            .listRowBackground(Color.surfaceRaised)
                    }
                    if note.speakers.count >= 2 {
                        NavigationLink {
                            MergeSpeakersView(speakers: note.speakers) { chosen, into in
                                dismiss()
                                onMerge(chosen, into)
                            }
                        } label: {
                            Label("Merge speakers…", systemImage: "arrow.triangle.merge")
                        }
                        .listRowBackground(Color.surfaceRaised)
                    }
                } footer: {
                    Text("To change who said one part, tap that part's speaker icon in the transcript.")
                }
            }
            .scrollContentBackground(.hidden)
            .background(Color.paper)
            .navigationTitle("Speakers")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") {
                        note.speakers.forEach(save)
                        dismiss()
                    }
                }
            }
            .confirmationDialog("How many people were talking?", isPresented: $choosingCount, titleVisibility: .visible) {
                Button("Work it out") { identify(0) }
                ForEach(2...6, id: \.self) { count in
                    Button("\(count) people") { identify(Int32(count)) }
                }
            }
        }
        .presentationDetents([.medium, .large])
    }

    private func name(_ speaker: SpeakerLabelValue) -> Binding<String> {
        Binding(get: { names[speaker.index] ?? speaker.name }, set: { names[speaker.index] = $0 })
    }

    /// Only a name that changed.
    private func save(_ speaker: SpeakerLabelValue) {
        guard let edited = names[speaker.index]?.trimmingCharacters(in: .whitespaces),
              edited != speaker.name else { return }
        names[speaker.index] = nil
        onRename(speaker.index, edited)
    }

    private func identify(_ count: Int32) {
        note.speakers.forEach(save)
        dismiss()
        onIdentify(count)
    }
}

/// Pick the speakers who are really one person; they take the first one's name.
struct MergeSpeakersView: View {
    let speakers: [SpeakerLabelValue]
    let onMerge: ([Int32], Int32) -> Void
    @State private var selection: Set<Int32> = []

    var body: some View {
        List {
            Section {
                ForEach(speakers) { speaker in
                    Button {
                        if selection.contains(speaker.index) { selection.remove(speaker.index) } else { selection.insert(speaker.index) }
                    } label: {
                        HStack(spacing: 12) {
                            Image(systemName: selection.contains(speaker.index) ? "checkmark.circle.fill" : "circle")
                                .foregroundStyle(selection.contains(speaker.index) ? Color.accentColor : Color.inkSoft)
                            SpeakerAvatar(initials: speakerInitials(speaker, index: speaker.index), index: speaker.index, size: 28)
                            Text(speaker.displayName).foregroundStyle(Color.ink)
                        }
                    }
                    .listRowBackground(Color.surfaceRaised)
                }
            } footer: {
                Text("Pick the speakers who are really one person. They take the first one's name.")
            }
        }
        .scrollContentBackground(.hidden)
        .background(Color.paper)
        .navigationTitle("Merge speakers")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .confirmationAction) {
                Button("Merge") {
                    let chosen = selection.sorted()
                    if let into = chosen.first { onMerge(chosen, into) }
                }
                .disabled(selection.count < 2)
            }
        }
    }
}
