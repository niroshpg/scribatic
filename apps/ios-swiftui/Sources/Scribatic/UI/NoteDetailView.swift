import LinkPresentation
import SwiftUI

/// One saved note: its transcript by speaker, and everything that can be done
/// with it — playing it, naming speakers, re-identifying them, sharing, and
/// deleting the recording while keeping the words.
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
    @State private var blockMenu: TranscriptBlock?
    @State private var settingCount = false
    @State private var merging = false
    @State private var mergeSelection: Set<Int32> = []

    private var isBusy: Bool { model.busyNoteID == noteID }

    var body: some View {
        Group {
            if let note {
                transcript(note)
            } else if loaded {
                EmptyStateView(title: "Note not found", message: "It may have been deleted.", failure: true)
            } else {
                ProgressView()
            }
        }
        .navigationBarTitleDisplayMode(.inline)
        .frame(maxWidth: 720)
        .frame(maxWidth: .infinity)
        .background(Color.paper)
        .toolbar { if let note { toolbar(note) } }
        .overlay {
            if isBusy {
                ProgressView("Identifying speakers")
                    .font(.uiLabel)
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
        .confirmationDialog(blockMenuTitle, isPresented: blockMenuBinding, titleVisibility: .visible,
                            presenting: blockMenu) { block in
            if let note {
                ForEach(note.speakers.filter { $0.index != block.speaker }) { other in
                    Button("\(other.displayName)") {
                        Task { await model.setBlockSpeaker(noteID, segments: block.segmentIds, speaker: other.index) }
                    }
                }
                Button("Someone new") {
                    Task { await model.setBlockSpeaker(noteID, segments: block.segmentIds, speaker: -1) }
                }
                if let current = note.speakers.first(where: { $0.index == block.speaker }) {
                    Button("Rename \(current.displayName)…") {
                        newName = current.name
                        renaming = current
                    }
                }
            }
        } message: { _ in
            Text("Who said this part?")
        }
        .confirmationDialog("How many people were talking?", isPresented: $settingCount, titleVisibility: .visible) {
            Button("Work it out") { reidentify(0) }
            ForEach(2...6, id: \.self) { count in
                Button("\(count) people") { reidentify(Int32(count)) }
            }
        }
        .sheet(isPresented: $merging) { mergeSheet }
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

    private var blockMenuBinding: Binding<Bool> {
        Binding(get: { blockMenu != nil }, set: { if !$0 { blockMenu = nil } })
    }

    private var blockMenuTitle: String {
        guard let block = blockMenu, let note else { return "Speaker" }
        return note.speakers.first(where: { $0.index == block.speaker })?.displayName ?? "Speaker"
    }

    private var mergeSheet: some View {
        NavigationStack {
            List {
                Section {
                    ForEach(note?.speakers ?? []) { speaker in
                        Button {
                            if mergeSelection.contains(speaker.index) { mergeSelection.remove(speaker.index) }
                            else { mergeSelection.insert(speaker.index) }
                        } label: {
                            HStack(spacing: 12) {
                                Image(systemName: mergeSelection.contains(speaker.index) ? "checkmark.circle.fill" : "circle")
                                    .foregroundStyle(mergeSelection.contains(speaker.index) ? Color.accentColor : Color.inkSoft)
                                SpeakerAvatar(initials: speakerInitials(speaker, index: speaker.index), index: speaker.index, size: 28)
                                Text(speaker.displayName).foregroundStyle(Color.ink)
                            }
                        }
                    }
                } footer: {
                    Text("Pick the speakers who are really one person. They take the first one's name.")
                }
            }
            .navigationTitle("Merge speakers")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { merging = false } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Merge") {
                        let chosen = mergeSelection.sorted()
                        merging = false
                        if let into = chosen.first {
                            Task { await model.mergeSpeakers(noteID, speakers: chosen, into: into) }
                        }
                    }
                    .disabled(mergeSelection.count < 2)
                }
            }
        }
        .presentationDetents([.medium, .large])
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
            header(note)
                .listRowBackground(Color.paper)
                .listRowSeparator(.hidden)

            if note.audioURL != nil {
                player(note)
                    .listRowBackground(Color.paper)
                    .listRowSeparator(.hidden)
            }

            // Headers here are ordinary rows, not pinned section headers: a
            // pinned "Speakers" stays on screen after its chips have scrolled
            // away, and sits over them under the toolbar.
            if !note.speakers.isEmpty && !note.isLecture {
                headerRow("Speakers")
                VStack(alignment: .leading, spacing: 8) {
                    FlowLayout(spacing: 8) {
                        ForEach(note.speakers) { speaker in
                            SpeakerChip(speaker: speaker) {
                                newName = speaker.name
                                renaming = speaker
                            }
                        }
                    }
                    HStack(spacing: 14) {
                        Text("\(note.speakerCount == 1 ? "1 speaker" : "\(note.speakerCount) speakers") · not right?")
                            .font(.uiCaption)
                            .foregroundStyle(Color.inkMuted)
                        Spacer()
                        Button("Set how many") { settingCount = true }
                            .font(.uiLabel)
                            .disabled(note.audioURL == nil)
                        if note.speakerCount >= 2 {
                            Button("Merge") {
                                mergeSelection = []
                                merging = true
                            }
                            .font(.uiLabel)
                        }
                    }
                    .buttonStyle(.borderless)
                    Text("Tap a name to rename. Tap a speaker's icon in the transcript to move that part to someone else.")
                        .font(.uiCaption)
                        .foregroundStyle(Color.inkSoft)
                }
                .listRowBackground(Color.paper)
                .listRowSeparator(.hidden)
            }

            headerRow("Transcript")
            if note.segments.isEmpty {
                Text("Nothing was transcribed.")
                    .font(.uiBodySmall)
                    .foregroundStyle(Color.inkMuted)
                    .listRowBackground(Color.paper)
                    .listRowSeparator(.hidden)
            }
            // A discussion is a card per turn, with whose turn it is; a
            // lecture, one voice throughout, is a card per paragraph.
            ForEach(note.isLecture ? lectureBlocks(note.segments) : discussionBlocks(note.segments)) { block in
                Group {
                    if note.isLecture {
                        LectureCard(block: block)
                    } else {
                        DiscussionCard(block: block,
                                       label: note.speakers.first(where: { $0.index == block.speaker })) {
                            blockMenu = block
                        }
                    }
                }
                .listRowInsets(EdgeInsets(top: 5, leading: 16, bottom: 5, trailing: 16))
                .listRowBackground(Color.paper)
                .listRowSeparator(.hidden)
            }
        }
        .listStyle(.plain)
        .environment(\.defaultMinListRowHeight, 0)
        .scrollContentBackground(.hidden)
    }

    private func headerRow(_ title: String) -> some View {
        SectionHeader(title: title)
            .padding(.top, 14)
            .listRowBackground(Color.paper)
            .listRowSeparator(.hidden)
    }

    private func header(_ note: NoteDetailValue) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(note.title)
                .font(.displayMedium)
                .foregroundStyle(Color.ink)
                .accessibilityAddTraits(.isHeader)
            FlowLayout(spacing: 12) {
                MetaChip(systemImage: "clock", text: timestamp(note.durationMs))
                MetaChip(systemImage: "character.bubble", text: languageName(note.language))
                if note.speakerCount > 0 {
                    MetaChip(systemImage: "person.2", text: note.speakerCount == 1 ? "1 speaker" : "\(note.speakerCount) speakers")
                }
                if note.audioURL != nil {
                    MetaBadge(systemImage: "lock.shield", text: "On this device")
                } else {
                    MetaChip(systemImage: "waveform.slash", text: "Audio deleted")
                }
            }
            if model.finishingNoteID == note.id {
                VStack(alignment: .leading, spacing: 6) {
                    let progress = model.finishingProgress
                    Text(progress >= 0 ? "\(model.finishingStep) — \(Int(progress * 100))%" : "\(model.finishingStep)…")
                        .font(.uiLabel)
                        .foregroundStyle(Color.inkMuted)
                    if progress >= 0 {
                        ProgressView(value: Double(max(progress, 0.02)))
                    } else {
                        ProgressView().frame(maxWidth: .infinity, alignment: .leading)
                    }
                    Text("You're reading the live preview; the final transcript replaces it when this is done.")
                        .font(.uiCaption)
                        .foregroundStyle(Color.inkSoft)
                }
            } else if model.canRefine && !note.refined && note.audioURL != nil {
                HStack {
                    Text("Live preview").font(.uiLabel).foregroundStyle(Color.inkMuted)
                    Spacer()
                    Button("Improve transcript") { model.finishNote(note.id) }
                        .font(.uiLabel)
                        .buttonStyle(.borderless)
                }
            }
        }
        .padding(.vertical, 4)
    }

    /// Playback where it can be seen, rather than inside the More menu.
    private func player(_ note: NoteDetailValue) -> some View {
        let playing = model.playingNoteID == note.id
        return HStack(spacing: 12) {
            Button {
                if playing { model.stopPlayback() } else { model.play(note) }
            } label: {
                Image(systemName: playing ? "stop.fill" : "play.fill")
                    .font(.system(size: 20, weight: .semibold))
                    .foregroundStyle(Color.paper)
                    .frame(width: 44, height: 44)
                    .background(Color.ink, in: Circle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel(playing ? "Stop playback" : "Play recording")
            VStack(alignment: .leading, spacing: 2) {
                Text(playing ? "Playing" : "Play recording").font(.uiLabel).foregroundStyle(Color.ink)
                Text(timestamp(note.durationMs)).font(.timestamp).foregroundStyle(Color.inkMuted)
            }
            .accessibilityHidden(true)
            Spacer()
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 10)
        .background(Color.surfaceRaised, in: RoundedRectangle(cornerRadius: 12))
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
                    if model.canIdentifySpeakers {
                        Menu {
                            Button("Work it out") { reidentify(0) }
                            ForEach(2...6, id: \.self) { count in
                                Button("\(count) people") { reidentify(Int32(count)) }
                            }
                        } label: {
                            Label("Identify speakers again", systemImage: "person.2.wave.2")
                        }
                        Divider()
                    }
                    Button("Delete recording…", systemImage: "waveform.slash", role: .destructive) {
                        confirmDeleteRecording = true
                    }
                }
                if !note.segments.isEmpty {
                    Button(note.isLecture ? "Show as a discussion" : "Show as a lecture",
                           systemImage: note.isLecture ? "person.2" : "graduationcap") {
                        Task { await model.setLayout(noteID, layout: note.isLecture ? "discussion" : "lecture") }
                    }
                    Divider()
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

/// A per-speaker colour, so turns can be told apart at a glance. Colour is
/// never the only signal: the name is always written alongside it.
struct SpeakerDot: View {
    let index: Int32

    var body: some View {
        Circle()
            .fill(Color.speaker(index))
            .frame(width: 8, height: 8)
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
