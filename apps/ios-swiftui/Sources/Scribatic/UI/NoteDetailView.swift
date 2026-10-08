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
    /// The speakers sheet, behind the header's "2 speakers".
    @State private var showingSpeakers = false
    /// "Preparing the recording — 40%" while a file for the share sheet is made.
    @State private var preparing: String?
    /// The card whose text is being corrected.
    @State private var editing: TranscriptBlock?
    /// The last correction, for its Undo.
    @State private var lastCorrection: Correction?
    /// Speaker identification or the accurate pass, waiting on the user's OK
    /// to drop their corrections.
    @State private var pendingRebuild: Rebuild?
    /// The card opened at (from a search result), outlined for a moment.
    @State private var marked: Int64?

    private struct Correction: Equatable {
        let segmentIds: [Int64]
        /// The card's text before, and whether that was itself a correction.
        let previous: String
        let previouslyEdited: Bool
        let id = UUID()
    }

    private enum Rebuild: Identifiable {
        case speakers(Int32)
        case refine
        var id: String {
            switch self {
            case let .speakers(count): "speakers-\(count)"
            case .refine: "refine"
            }
        }
    }

    private var isBusy: Bool { model.busyNoteID == noteID }

    var body: some View {
        withDialogs
        .sheet(item: $editing) { block in
            CardEditor(block: block, canPlay: note?.audioURL != nil,
                       playing: model.playingSpan == block.startMs,
                       onPlay: { toggleCardPlayback(block) },
                       onSave: { text in
                           editing = nil
                           Task {
                               if await model.editText(noteID, segments: block.segmentIds, text: text) {
                                   lastCorrection = Correction(segmentIds: block.segmentIds, previous: block.text,
                                                               previouslyEdited: block.edited)
                               }
                           }
                       },
                       onRestore: {
                           editing = nil
                           Task { await model.restoreText(noteID, segments: block.segmentIds) }
                       },
                       onCancel: { editing = nil })
        }
        .confirmationDialog("Discard your corrections?", isPresented: rebuildBinding, titleVisibility: .visible,
                            presenting: pendingRebuild) { rebuild in
            Button(rebuild.id == "refine" ? "Improve transcript" : "Identify speakers", role: .destructive) {
                run(rebuild)
            }
        } message: { _ in
            let count = note?.corrections ?? 0
            Text("This rebuilds the transcript from the recording, so your \(count == 1 ? "correction" : "\(count) corrections") to the text will be lost.")
        }
        .safeAreaInset(edge: .bottom) {
            if let correction = lastCorrection {
                HStack {
                    Text("Text corrected").font(.uiLabel).foregroundStyle(Color.paper)
                    Spacer()
                    Button("Undo") {
                        lastCorrection = nil
                        Task {
                            if correction.previouslyEdited {
                                await model.editText(noteID, segments: correction.segmentIds, text: correction.previous)
                            } else {
                                await model.restoreText(noteID, segments: correction.segmentIds)
                            }
                        }
                    }
                    .font(.uiLabel.weight(.semibold))
                    .foregroundStyle(Color.paper)
                }
                .padding(.horizontal, 16)
                .padding(.vertical, 12)
                .background(Color.ink, in: RoundedRectangle(cornerRadius: 12))
                .padding(.horizontal, 16)
                .padding(.bottom, 8)
                .transition(.move(edge: .bottom).combined(with: .opacity))
                .task(id: correction.id) {
                    try? await Task.sleep(for: .seconds(6))
                    if lastCorrection == correction { lastCorrection = nil }
                }
            }
        }
        .animation(.default, value: lastCorrection)
    }

    private var base: some View {
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
            if isBusy || preparing != nil {
                ProgressView(preparing ?? "Identifying speakers")
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
        .sheet(isPresented: $showingSpeakers) {
            if let note {
                SpeakersSheet(
                    note: note,
                    canIdentify: note.audioURL != nil && model.canIdentifySpeakers,
                    onRename: { index, name in Task { await model.renameSpeaker(noteID, speaker: index, name: name) } },
                    onIdentify: { count in
                        // After the sheet has gone: the corrections question
                        // can't present from a sheet that is dismissing.
                        Task {
                            try? await Task.sleep(for: .milliseconds(450))
                            reidentify(count)
                        }
                    },
                    onMerge: { chosen, into in Task { await model.mergeSpeakers(noteID, speakers: chosen, into: into) } }
                )
            }
        }
        .sheet(item: $sharing) { payload in
            ShareSheet(payload: payload) { sharing = nil }
                .presentationDetents([.medium, .large])
                .ignoresSafeArea()
        }
    }

    private var withDialogs: some View {
        base
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
                // Not just this part: everything one label said is another's.
                if let current = note.speakers.first(where: { $0.index == block.speaker }) {
                    ForEach(note.speakers.filter { $0.index != block.speaker }) { other in
                        Button("Everything \(current.displayName) said is \(other.displayName)") {
                            Task { await model.mergeSpeakers(noteID, speakers: [other.index, current.index], into: other.index) }
                        }
                    }
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

    private var rebuildBinding: Binding<Bool> {
        Binding(get: { pendingRebuild != nil }, set: { if !$0 { pendingRebuild = nil } })
    }

    private func toggleCardPlayback(_ block: TranscriptBlock) {
        guard let note else { return }
        if model.playingSpan == block.startMs {
            model.stopPlayback()
        } else {
            model.play(note, fromMs: block.startMs, toMs: block.endMs)
        }
    }

    /// Rebuilding from recognition drops corrections: asked first when there are any.
    private func rebuild(_ rebuild: Rebuild) {
        if (note?.corrections ?? 0) > 0 {
            pendingRebuild = rebuild
        } else {
            run(rebuild)
        }
    }

    private func run(_ rebuild: Rebuild) {
        switch rebuild {
        case let .speakers(count): Task { await model.identifySpeakers(noteID, expected: count) }
        case .refine: model.finishNote(noteID)
        }
    }

    private var renamingBinding: Binding<Bool> {
        Binding(get: { renaming != nil }, set: { if !$0 { renaming = nil } })
    }

    private func reload() async {
        note = await model.loadNote(noteID)
        loaded = true
        shareText = withSummary(await model.exportTranscript(noteID, anonymise: false), note?.summary ?? "")
        anonymisedShareText = await model.exportTranscript(noteID, anonymise: true)
    }

    /// The title and date lines, the summary (with its own headings), then
    /// the turns. Without names the summary is left out too: it names people.
    private func withSummary(_ transcript: String, _ summary: String) -> String {
        let summary = summary.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !transcript.isEmpty, !summary.isEmpty else { return transcript }
        let lines = transcript.components(separatedBy: "\n")
        let head = lines.prefix(2).joined(separator: "\n")
        let body = lines.dropFirst(2).joined(separator: "\n").trimmingCharacters(in: .newlines)
        return "\(head)\n\n\(summary)\n\nTranscript:\n\(body)"
    }

    // MARK: - Sharing files

    /// The transcript and summary as a PDF, for people who want a document.
    private func sharePDF(_ note: NoteDetailValue) {
        Task {
            preparing = "Preparing the PDF"
            defer { preparing = nil }
            let transcript = await model.exportTranscript(noteID, anonymise: false)
            let names = note.speakers.map(\.displayName)
            do {
                // On the main actor: UIKit's print formatters are, and laying
                // out even an hour's transcript takes well under a second.
                let url = try ShareFiles.target(name: note.title, extension: "pdf")
                try ShareFiles.writePDF(to: url, transcript: transcript, summary: note.summary, speakerNames: names)
                sharing = SharePayload(subject: note.title, file: url)
            } catch {
                model.noteError = "The PDF could not be made: \(error.localizedDescription)"
            }
        }
    }

    /// The recording: as a small M4A (AAC), or the original WAV as recorded.
    private func shareRecording(_ note: NoteDetailValue, compressed: Bool) {
        guard let wav = note.audioURL else { return }
        Task {
            preparing = "Preparing the recording"
            defer { preparing = nil }
            do {
                let url = try await Task.detached {
                    let url = try ShareFiles.target(name: note.title, extension: compressed ? "m4a" : "wav")
                    // A finished note's recording is already the M4A.
                    if compressed && RecordingFiles.isCompressed(wav) {
                        try FileManager.default.copyItem(at: wav, to: url)
                    } else if compressed {
                        var shown = -1
                        try ShareFiles.encodeM4A(wav: wav, to: url) { progress in
                            let percent = Int(progress * 100)
                            guard percent != shown else { return }
                            shown = percent
                            Task { @MainActor in preparing = "Preparing the recording — \(percent)%" }
                        }
                    } else {
                        try FileManager.default.copyItem(at: wav, to: url)
                    }
                    return url
                }.value
                sharing = SharePayload(subject: note.title, file: url)
            } catch {
                model.noteError = "The recording could not be prepared: \(error.localizedDescription)"
            }
        }
    }

    // MARK: - Transcript

    private func transcript(_ note: NoteDetailValue) -> some View {
        // A discussion is a card per turn, with whose turn it is; a lecture,
        // one voice throughout, is a card per paragraph.
        let blocks = note.isLecture ? lectureBlocks(note.segments) : discussionBlocks(note.segments)
        // Read here, in the body, so a change to it redraws the cards: a
        // read inside the List's lazily built rows alone isn't tracked.
        let marked = marked
        return ScrollViewReader { proxy in
            cards(note, blocks, marked: marked)
                .task(id: model.focus) {
                    guard let focus = model.focus, focus.noteID == noteID, !blocks.isEmpty else { return }
                    let card = (blocks.last { $0.startMs <= focus.atMs } ?? blocks[0]).id
                    model.focus = nil
                    // Not in this task: the list appears more than once as the
                    // note loads, and each time cancels the tasks attached to
                    // it, which would cut the scroll and the mark short.
                    Task { await showFocus(card, proxy) }
                }
        }
    }

    /// Scrolls to the card holding the moment the note was opened at, and
    /// outlines it for a moment.
    private func showFocus(_ card: Int64, _ proxy: ScrollViewProxy) async {
        // After the list has laid out its first rows; scrolling sooner is lost.
        try? await Task.sleep(for: .milliseconds(150))
        withAnimation { proxy.scrollTo(card, anchor: .top) }
        marked = card
        try? await Task.sleep(for: .seconds(2.5))
        withAnimation { if marked == card { marked = nil } }
    }

    private func cards(_ note: NoteDetailValue, _ blocks: [TranscriptBlock], marked: Int64?) -> some View {
        List {
            header(note)
                .listRowBackground(Color.paper)
                .listRowSeparator(.hidden)

            if note.audioURL != nil {
                player(note)
                    .listRowBackground(Color.paper)
                    .listRowSeparator(.hidden)
            }

            ForEach(Extensions.noteSections.indices, id: \.self) { index in
                Extensions.noteSections[index].content(note: note, host: model)
                    .listRowBackground(Color.paper)
                    .listRowSeparator(.hidden)
            }

            headerRow("Transcript")
            // Finishing rebuilds the transcript from recognition, so a
            // correction made meanwhile would be lost: none until it is done.
            let finishing = model.finishingNoteID == note.id
            if !note.segments.isEmpty {
                Text(finishing ? "You can correct the text once this is finished."
                               : "Touch and hold a part to correct its text\(note.audioURL != nil ? " or play it" : "").")
                    .font(.uiCaption)
                    .foregroundStyle(Color.inkSoft)
                    .listRowBackground(Color.paper)
                    .listRowSeparator(.hidden)
            }
            if note.segments.isEmpty {
                Text("Nothing was transcribed.")
                    .font(.uiBodySmall)
                    .foregroundStyle(Color.inkMuted)
                    .listRowBackground(Color.paper)
                    .listRowSeparator(.hidden)
            }
            ForEach(blocks) { block in
                let playing = model.playingSpan == block.startMs
                let onPlay: (() -> Void)? = note.audioURL != nil ? { toggleCardPlayback(block) } : nil
                Group {
                    if note.isLecture {
                        LectureCard(block: block, playing: playing, onPlay: onPlay)
                    } else {
                        DiscussionCard(block: block,
                                       label: note.speakers.first(where: { $0.index == block.speaker }),
                                       playing: playing, onPlay: onPlay) {
                            blockMenu = block
                        }
                    }
                }
                .overlay {
                    RoundedRectangle(cornerRadius: 16)
                        .stroke(Color.accent, lineWidth: 2)
                        .opacity(marked == block.id ? 1 : 0)
                }
                .id(block.id)
                .contextMenu {
                    Button("Edit text", systemImage: "pencil") { editing = block }
                        .disabled(finishing)
                    if let onPlay {
                        Button(playing ? "Stop" : "Play this part", systemImage: playing ? "stop.fill" : "play.fill",
                               action: onPlay)
                    }
                    if block.edited && !finishing {
                        Button("Restore original", systemImage: "arrow.uturn.backward") {
                            Task { await model.restoreText(noteID, segments: block.segmentIds) }
                        }
                    }
                    if !note.isLecture && block.speaker >= 0 {
                        Button("Change speaker…", systemImage: "person.crop.circle") { blockMenu = block }
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
                    // The way into the speakers: out of the transcript's way
                    // until wanted.
                    Button { showingSpeakers = true } label: {
                        HStack(spacing: 3) {
                            MetaChip(systemImage: "person.2",
                                     text: note.speakerCount == 1 ? "1 speaker" : "\(note.speakerCount) speakers",
                                     color: .accentStrong)
                            Image(systemName: "chevron.right")
                                .font(.system(size: 9, weight: .semibold))
                                .foregroundStyle(Color.accentStrong)
                        }
                    }
                    .buttonStyle(.plain)
                    .accessibilityHint("Name the speakers, set how many there were, or merge them")
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
                    Button("Improve transcript") { rebuild(.refine) }
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
                Button("Share as PDF", systemImage: "doc.richtext") {
                    sharePDF(note)
                }
                if note.speakerCount > 0 {
                    Button("Share without names", systemImage: "person.crop.circle.badge.questionmark") {
                        sharing = SharePayload(subject: note.title, text: anonymisedShareText)
                    }
                }
                if note.audioURL != nil {
                    Divider()
                    // Sizes from the length: AAC at 48 kbit/s, WAV at 16 kHz × 4 bytes.
                    let seconds = Double(note.durationMs) / 1000
                    Menu {
                        Button {
                            shareRecording(note, compressed: true)
                        } label: {
                            Text("Compressed (M4A)")
                            Text("About \(fileSize(seconds * 6_000)) · plays anywhere")
                        }
                        // Kept compressed once finished: the original WAV is gone.
                        if let audio = note.audioURL, !RecordingFiles.isCompressed(audio) {
                            Button {
                                shareRecording(note, compressed: false)
                            } label: {
                                Text("Original (WAV)")
                                Text("About \(fileSize(seconds * 64_000)) · as recorded")
                            }
                        }
                    } label: {
                        Label("Share recording", systemImage: "waveform")
                    }
                }
            } label: {
                Label("Share", systemImage: "square.and.arrow.up")
            }
            .disabled(shareText.isEmpty || preparing != nil)

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
        rebuild(.speakers(expected))
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

/// Text, or a file made by ShareFiles, for the share sheet.
struct SharePayload: Identifiable {
    let id = UUID()
    let subject: String
    var text: String = ""
    var file: URL?
}

/// "320 KB", "14 MB", "1.2 GB": roughly, for a choice between sizes.
private func fileSize(_ bytes: Double) -> String {
    ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .file)
}

/// The system share sheet around text or a file. The app sends nothing
/// itself: whichever app the user picks does, which is why sharing needs no
/// network.
private struct ShareSheet: UIViewControllerRepresentable {
    let payload: SharePayload
    let onFinish: () -> Void

    func makeUIViewController(context: Context) -> UIActivityViewController {
        let item: Any = payload.file ?? TextItem(subject: payload.subject, text: payload.text)
        let controller = UIActivityViewController(activityItems: [item], applicationActivities: nil)
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
