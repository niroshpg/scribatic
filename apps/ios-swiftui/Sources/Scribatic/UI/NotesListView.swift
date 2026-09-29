import SwiftUI

/// Root screen: every note on this device, newest first, grouped by day.
struct NotesListView: View {
    @Bindable var model: TranscriptionModel
    @State private var pendingDelete: [Int64] = []

    private var engineStopped: Bool { model.failureMessage != nil && model.notes.isEmpty }

    var body: some View {
        content
            .frame(maxWidth: 720)
            .frame(maxWidth: .infinity)
            .background(Color.paper)
            .navigationTitle("Notes")
            .toolbar {
                ToolbarItem(placement: .primaryAction) {
                    Button {
                        model.showingModels = true
                    } label: {
                        Label("Models", systemImage: "cpu")
                    }
                }
            }
            .safeAreaInset(edge: .bottom) {
                if !engineStopped {
                    // One tap: pushing the recorder starts capture.
                    RecordButton(stop: false, label: "New recording", floating: true) {
                        model.path.append(.recorder)
                    }
                    .disabled(model.phase != .ready)
                    .padding(.bottom, 4)
                }
            }
            .confirmationDialog("Delete this note?", isPresented: deleteBinding, titleVisibility: .visible) {
                Button("Delete note", role: .destructive) {
                    let ids = pendingDelete
                    Task { for id in ids { await model.deleteNote(id) } }
                }
            } message: {
                Text("The transcript, the speaker names and the recording are all removed from this device.")
            }
    }

    private var deleteBinding: Binding<Bool> {
        Binding(get: { !pendingDelete.isEmpty }, set: { if !$0 { pendingDelete = [] } })
    }

    @ViewBuilder
    private var content: some View {
        if let failure = model.failureMessage, model.notes.isEmpty {
            EmptyStateView(title: "Engine stopped", message: failure, failure: true) {
                Button {
                    Task { await model.prepare() }
                } label: {
                    Label("Try again", systemImage: "arrow.clockwise")
                }
                .buttonStyle(PrimaryButtonStyle())
                Button {
                    UIPasteboard.general.string = failure
                } label: {
                    Label("Copy details", systemImage: "doc.on.doc")
                }
                .buttonStyle(SecondaryButtonStyle())
            }
        } else if model.phase == .starting {
            ProgressView("Preparing")
                .font(.uiLabel)
                .foregroundStyle(Color.inkMuted)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else if model.notes.isEmpty {
            EmptyStateView(
                title: "No notes yet",
                message: "Record a conversation and it appears here, transcribed on this device with each speaker marked.",
                aside: "Nothing is uploaded."
            )
        } else {
            List {
                ForEach(dayGroups, id: \.title) { group in
                    Section {
                        ForEach(group.notes) { note in
                            NavigationLink(value: TranscriptionModel.Route.note(note.id)) {
                                NoteRow(note: note)
                            }
                            .listRowBackground(Color.paper)
                            .listRowSeparatorTint(Color.line)
                        }
                        // The swipe asks, it never deletes: the same
                        // confirmation as the note screen decides.
                        .onDelete { offsets in
                            pendingDelete = offsets.map { group.notes[$0].id }
                        }
                    } header: {
                        SectionHeader(title: group.title, trailing: group.notes.count == 1 ? "1 note" : "\(group.notes.count) notes")
                            .pinnedHeaderGround()
                    }
                }
            }
            .listStyle(.plain)
            .scrollContentBackground(.hidden)
        }
    }

    private struct DayGroup {
        let title: String
        let notes: [NoteSummaryValue]
    }

    private var dayGroups: [DayGroup] {
        let calendar = Calendar.current
        var groups: [DayGroup] = []
        var current: (day: Date, notes: [NoteSummaryValue])?
        for note in model.notes {
            let day = calendar.startOfDay(for: note.createdAt)
            if current?.day == day {
                current?.notes.append(note)
            } else {
                if let current { groups.append(DayGroup(title: Self.title(for: current.day), notes: current.notes)) }
                current = (day, [note])
            }
        }
        if let current { groups.append(DayGroup(title: Self.title(for: current.day), notes: current.notes)) }
        return groups
    }

    private static func title(for day: Date) -> String {
        let calendar = Calendar.current
        if calendar.isDateInToday(day) { return "Today" }
        if calendar.isDateInYesterday(day) { return "Yesterday" }
        let sameYear = calendar.isDate(day, equalTo: .now, toGranularity: .year)
        return sameYear
            ? day.formatted(.dateTime.weekday(.wide).day().month(.wide))
            : day.formatted(.dateTime.weekday(.wide).day().month(.wide).year())
    }
}

/// Leads with the time and the first words spoken: notes are titled by date,
/// and already sit under a day header.
private struct NoteRow: View {
    let note: NoteSummaryValue

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(alignment: .firstTextBaseline) {
                Text(note.createdAt, format: .dateTime.hour().minute())
                    .font(.uiTitle)
                    .foregroundStyle(Color.ink)
                Spacer()
                MetaChip(systemImage: "clock", text: timestamp(note.durationMs))
            }
            if !note.preview.isEmpty {
                Text(note.preview)
                    .font(.uiBodySmall)
                    .foregroundStyle(Color.inkMuted)
                    .lineLimit(2)
            }
            if note.speakerCount > 0 || !note.hasAudio {
                HStack(spacing: 12) {
                    if note.speakerCount > 0 {
                        MetaChip(systemImage: "person.2", text: note.speakerCount == 1 ? "1 speaker" : "\(note.speakerCount) speakers")
                    }
                    if !note.hasAudio {
                        // Worth showing in the list: it means the note can no
                        // longer be played or have its speakers re-identified.
                        MetaChip(systemImage: "waveform.slash", text: "Audio deleted")
                    }
                }
            }
        }
        .padding(.vertical, 4)
        .accessibilityElement(children: .combine)
    }
}
