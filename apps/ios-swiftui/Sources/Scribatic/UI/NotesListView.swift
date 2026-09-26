import SwiftUI

/// Root screen: every note on this device, newest first.
struct NotesListView: View {
    @Bindable var model: TranscriptionModel

    var body: some View {
        content
            .navigationTitle("Notes")
            .frame(maxWidth: 720)
            .frame(maxWidth: .infinity)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button("Models") { model.showingModels = true }
                }
                ToolbarItem(placement: .primaryAction) {
                    Button {
                        model.path.append(.recorder)
                    } label: {
                        Label("New recording", systemImage: "mic.fill")
                    }
                    .tint(.scribaticAccent)
                    .disabled(model.phase != .ready)
                }
            }
    }

    @ViewBuilder
    private var content: some View {
        if let failure = model.failureMessage, model.notes.isEmpty {
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
        } else if model.phase == .starting {
            ProgressView("Preparing")
        } else if model.notes.isEmpty {
            ContentUnavailableView {
                Label("No notes yet", systemImage: "waveform")
            } description: {
                Text("Record a conversation and it appears here, transcribed on this device with each speaker marked.")
            } actions: {
                Button {
                    model.path.append(.recorder)
                } label: {
                    Label("New recording", systemImage: "mic.fill")
                }
                .buttonStyle(.borderedProminent)
                .tint(.scribaticAccent)
            }
        } else {
            List {
                ForEach(model.notes) { note in
                    NavigationLink(value: TranscriptionModel.Route.note(note.id)) {
                        NoteRow(note: note)
                    }
                }
                .onDelete { offsets in
                    let ids = offsets.map { model.notes[$0].id }
                    Task { for id in ids { await model.deleteNote(id) } }
                }
            }
            .listStyle(.plain)
        }
    }
}

private struct NoteRow: View {
    let note: NoteSummaryValue

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(note.title)
                .font(.headline)
            if !note.preview.isEmpty {
                Text(note.preview)
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
            }
            HStack(spacing: 12) {
                Label(timestamp(note.durationMs), systemImage: "clock")
                if note.speakerCount > 0 {
                    Label("^[\(note.speakerCount) speaker](inflect: true)", systemImage: "person.2")
                }
                if !note.hasAudio {
                    // Worth showing in the list: it means the note can no
                    // longer be played or have its speakers re-identified.
                    Label("Audio deleted", systemImage: "waveform.slash")
                }
            }
            .font(.caption)
            .foregroundStyle(.tertiary)
            .labelStyle(.titleAndIcon)
        }
        .padding(.vertical, 4)
        .accessibilityElement(children: .combine)
    }
}
