import LinkPresentation
import SwiftUI

/// One saved note: its transcript by speaker, and everything that can be done
/// with it — naming speakers, re-identifying them, sharing, and deleting the
/// recording while keeping the words.
struct NoteDetailView: View {
    @Bindable var model: TranscriptionModel
    let noteID: Int64

    @State private var note: NoteDetailValue?
    @State private var loaded = false
    @State private var shareText = ""
    @State private var anonymisedShareText = ""
    @State private var renaming: SpeakerLabelValue?
    @State private var newName = ""
    @State private var confirmDeleteRecording = false
    @State private var confirmDeleteNote = false
    @State private var sharing: SharePayload?

    private var isBusy: Bool { model.busyNoteID == noteID }

    var body: some View {
        Group {
            if let note {
                transcript(note)
            } else if loaded {
                ContentUnavailableView("Note not found", systemImage: "doc.questionmark")
            } else {
                ProgressView()
            }
        }
        .navigationTitle(note?.title ?? "")
        .navigationBarTitleDisplayMode(.inline)
        .frame(maxWidth: 720)
        .frame(maxWidth: .infinity)
        .toolbar { if let note { toolbar(note) } }
        .overlay {
            if isBusy {
                ProgressView("Identifying speakers")
                    .padding(24)
                    .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
            }
        }
        .task(id: model.noteRevision) { await reload() }
        // Presented here, from the note screen, and not by a ShareLink inside
        // the toolbar Menu: that presents while the menu is still dismissing,
        // anchors to the vanished menu, and shows an empty sheet at the top
        // edge — a popover pointing at nothing on iPad.
        .sheet(item: $sharing) { payload in
            ShareSheet(payload: payload) { sharing = nil }
                .presentationDetents([.medium, .large])
                .ignoresSafeArea()
        }
        .alert("Name this speaker", isPresented: renamingBinding, presenting: renaming) { speaker in
            TextField(speaker.displayName, text: $newName)
                .textInputAutocapitalization(.words)
            Button("Save") {
                Task { await model.renameSpeaker(noteID, speaker: speaker.index, name: newName) }
            }
            Button("Cancel", role: .cancel) {}
        } message: { _ in
            Text("Only on this device, and only for this note. Leave it empty to go back to the numbered label.")
        }
        .confirmationDialog("Delete the recording?", isPresented: $confirmDeleteRecording,
                            titleVisibility: .visible) {
            Button("Delete recording", role: .destructive) {
                Task { await model.deleteRecording(noteID) }
            }
        } message: {
            Text("The transcript and speaker names stay. The audio is removed from this device and cannot be played again, and speakers can no longer be re-identified.")
        }
        .confirmationDialog("Delete this note?", isPresented: $confirmDeleteNote,
                            titleVisibility: .visible) {
            Button("Delete note", role: .destructive) {
                Task { await model.deleteNote(noteID) }
            }
        } message: {
            Text("The transcript, the speaker names and the recording are all removed from this device.")
        }
    }

    private var renamingBinding: Binding<Bool> {
        Binding(get: { renaming != nil }, set: { if !$0 { renaming = nil } })
    }

    private func reload() async {
        note = await model.loadNote(noteID)
        loaded = true
        shareText = await model.exportTranscript(noteID, anonymise: false)
        anonymisedShareText = await model.exportTranscript(noteID, anonymise: true)
    }

    // MARK: - Transcript

    private func transcript(_ note: NoteDetailValue) -> some View {
        List {
            Section {
                LabeledContent("Length", value: timestamp(note.durationMs))
                LabeledContent("Recording") {
                    if note.audioURL != nil {
                        Text("On this device")
                    } else {
                        Label("Deleted", systemImage: "waveform.slash")
                    }
                }
            }

            if !note.speakers.isEmpty {
                Section {
                    ForEach(note.speakers) { speaker in
                        Button {
                            newName = speaker.name
                            renaming = speaker
                        } label: {
                            HStack {
                                SpeakerDot(index: speaker.index)
                                Text(speaker.displayName)
                                    .foregroundStyle(.primary)
                                Spacer()
                                Image(systemName: "pencil")
                                    .foregroundStyle(.secondary)
                            }
                        }
                        .accessibilityHint("Gives this speaker a name")
                    }
                } header: {
                    Text("Speakers")
                } footer: {
                    Text("Tap to name. If one person was split into two, give both the same name.")
                }
            }

            Section("Transcript") {
                if note.segments.isEmpty {
                    Text("Nothing was transcribed.")
                        .foregroundStyle(.secondary)
                }
                ForEach(Array(note.segments.enumerated()), id: \.offset) { index, segment in
                    SegmentRow(
                        segment: segment,
                        speakerName: note.speakerName(segment.speaker),
                        // A name only where the speaker changes: a run of
                        // one person's sentences reads as a paragraph.
                        showsSpeaker: index == 0 || note.segments[index - 1].speaker != segment.speaker
                    )
                }
            }
        }
        .listStyle(.insetGrouped)
    }

    // MARK: - Toolbar

    @ToolbarContentBuilder
    private func toolbar(_ note: NoteDetailValue) -> some ToolbarContent {
        ToolbarItemGroup(placement: .primaryAction) {
            Menu {
                Button("Share transcript", systemImage: "square.and.arrow.up") {
                    sharing = SharePayload(subject: note.title, text: shareText)
                }
                if note.speakerCount > 0 {
                    Button("Share without names", systemImage: "person.crop.circle.badge.questionmark") {
                        sharing = SharePayload(subject: note.title, text: anonymisedShareText)
                    }
                }
            } label: {
                Label("Share", systemImage: "square.and.arrow.up")
            }
            .disabled(shareText.isEmpty)

            Menu {
                if note.audioURL != nil {
                    if model.playingNoteID == note.id {
                        Button("Stop playback", systemImage: "stop.fill") { model.stopPlayback() }
                    } else {
                        Button("Play recording", systemImage: "play.fill") { model.play(note) }
                    }

                    if model.canIdentifySpeakers {
                        Menu {
                            Button("Work it out") { reidentify(0) }
                            ForEach(2...6, id: \.self) { count in
                                Button("\(count) people") { reidentify(Int32(count)) }
                            }
                        } label: {
                            Label("Identify speakers again", systemImage: "person.2.wave.2")
                        }
                    }

                    Divider()
                    Button("Delete recording…", systemImage: "waveform.slash", role: .destructive) {
                        confirmDeleteRecording = true
                    }
                }
                Button("Delete note…", systemImage: "trash", role: .destructive) {
                    confirmDeleteNote = true
                }
            } label: {
                Label("More", systemImage: "ellipsis.circle")
            }
            .disabled(isBusy)
        }
    }

    private func reidentify(_ expected: Int32) {
        Task { await model.identifySpeakers(noteID, expected: expected) }
    }
}

private struct SegmentRow: View {
    let segment: TranscriptSegmentValue
    let speakerName: String?
    let showsSpeaker: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            if showsSpeaker {
                HStack(spacing: 6) {
                    if let speakerName {
                        SpeakerDot(index: segment.speaker)
                        Text(speakerName)
                            .font(.subheadline.weight(.semibold))
                            .foregroundStyle(SpeakerDot.color(segment.speaker))
                    }
                    Spacer()
                    Text(timestamp(segment.startMs))
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.tertiary)
                }
            }
            Text(segment.text)
                .font(.body)
                .textSelection(.enabled)
        }
        .padding(.vertical, 2)
        .accessibilityElement(children: .combine)
    }
}

/// A per-speaker colour, so turns can be told apart at a glance. Colour is
/// never the only signal: the name is always written alongside it.
struct SpeakerDot: View {
    let index: Int32

    static func color(_ index: Int32) -> Color {
        let palette: [Color] = [.scribaticAccent, .blue, .green, .purple, .pink, .teal, .indigo, .brown]
        guard index >= 0 else { return .secondary }
        return palette[Int(index) % palette.count]
    }

    var body: some View {
        Circle()
            .fill(Self.color(index))
            .frame(width: 10, height: 10)
            .accessibilityHidden(true)
    }
}

// MARK: - Share sheet

struct SharePayload: Identifiable {
    let id = UUID()
    let subject: String
    let text: String
}

/// The system share sheet around plain text. The app sends nothing itself:
/// whichever app the user picks does, which is why sharing needs no network.
private struct ShareSheet: UIViewControllerRepresentable {
    let payload: SharePayload
    let onFinish: () -> Void

    func makeUIViewController(context: Context) -> UIActivityViewController {
        let controller = UIActivityViewController(
            activityItems: [TextItem(subject: payload.subject, text: payload.text)],
            applicationActivities: nil
        )
        controller.completionWithItemsHandler = { _, _, _, _ in onFinish() }
        return controller
    }

    func updateUIViewController(_ controller: UIActivityViewController, context: Context) {}
}

/// Supplies the text, plus a subject line for targets that have one (Mail).
private final class TextItem: NSObject, UIActivityItemSource {
    let subject: String
    let text: String

    init(subject: String, text: String) {
        self.subject = subject
        self.text = text
    }

    func activityViewControllerPlaceholderItem(_ controller: UIActivityViewController) -> Any { text }

    func activityViewController(_ controller: UIActivityViewController,
                                itemForActivityType activityType: UIActivity.ActivityType?) -> Any? { text }

    func activityViewController(_ controller: UIActivityViewController,
                                subjectForActivityType activityType: UIActivity.ActivityType?) -> String { subject }

    /// The sheet's header: the note's title instead of a blank placeholder.
    func activityViewControllerLinkMetadata(_ controller: UIActivityViewController) -> LPLinkMetadata? {
        let metadata = LPLinkMetadata()
        metadata.title = subject
        return metadata
    }
}
