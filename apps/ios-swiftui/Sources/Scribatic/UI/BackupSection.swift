import SwiftUI

/// Backing up to a place the user picks, and restoring. The file is encrypted
/// with a passphrase only the user knows; Scribatic never uploads it, and
/// keeps no copy of the passphrase.
struct BackupSection: View {
    @Bindable var model: TranscriptionModel
    @AppStorage("backupRecordings") private var includeRecordings = true
    @State private var asking: Purpose?
    @State private var passphrase = ""
    @State private var choosingFolder = false
    @State private var choosingBackup = false
    @State private var chosenBackup: URL?
    @State private var outcome: String?

    enum Purpose: Identifiable {
        case backUp, restore
        var id: Self { self }
    }

    var body: some View {
        Section {
            Toggle(isOn: $includeRecordings) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Include recordings").font(.uiBody).foregroundStyle(Color.ink)
                    Text("Without them the backup is far smaller, but restored notes can't be played.")
                        .font(.uiBodySmall).foregroundStyle(Color.inkMuted)
                }
            }
            .listRowBackground(Color.surfaceRaised)
            Button("Back up now…", systemImage: "externaldrive.badge.plus") { asking = .backUp }
                .listRowBackground(Color.surfaceRaised)
            Button("Restore from a backup…", systemImage: "arrow.counterclockwise") { choosingBackup = true }
                .listRowBackground(Color.surfaceRaised)
        } header: {
            SectionHeader(title: "Backup", trailing: model.lastBackup.map { "Last: \($0.formatted(date: .abbreviated, time: .omitted))" })
        } footer: {
            Text("You choose where the backup goes: this device, a drive, or a cloud folder you use. It's encrypted with your passphrase, so nobody can read it without that, including Scribatic. If you forget the passphrase, the backup can't be opened.")
                .font(.uiCaption)
                .foregroundStyle(Color.inkMuted)
        }
        .disabled(model.backupProgress != nil || !model.canBackUp)
        .sheet(item: $asking) { purpose in
            PassphraseSheet(confirming: purpose == .backUp) { entered in
                asking = nil
                passphrase = entered
                switch purpose {
                case .backUp: choosingFolder = true
                case .restore:
                    if let file = chosenBackup {
                        Task { outcome = await model.restore(from: file, passphrase: entered); passphrase = "" }
                    }
                }
            } onCancel: { asking = nil }
        }
        .fileImporter(isPresented: $choosingFolder, allowedContentTypes: [.folder]) { result in
            if case let .success(folder) = result {
                let entered = passphrase
                Task {
                    outcome = await model.backUp(to: folder, passphrase: entered, includeRecordings: includeRecordings)
                    passphrase = ""
                }
            } else {
                passphrase = ""
            }
        }
        .fileImporter(isPresented: $choosingBackup, allowedContentTypes: [.item]) { result in
            if case let .success(file) = result {
                chosenBackup = file
                asking = .restore
            }
        }
        .alert("Backup", isPresented: Binding(get: { outcome != nil }, set: { if !$0 { outcome = nil } })) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(outcome ?? "")
        }
    }
}

/// The backup's passphrase: twice when making one, once to open one.
struct PassphraseSheet: View {
    let confirming: Bool
    let onContinue: (String) -> Void
    let onCancel: () -> Void
    @State private var first = ""
    @State private var second = ""

    private var problem: String? {
        if first.count < Backup.minimumPassphrase { return "At least \(Backup.minimumPassphrase) characters." }
        if confirming && first != second { return "The two don't match." }
        return nil
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    SecureField("Passphrase", text: $first)
                    if confirming { SecureField("Again", text: $second) }
                } footer: {
                    Text(confirming
                         ? "Keep it somewhere safe, such as a password manager. Without it the backup can't be opened, and Scribatic can't recover it."
                         : "The passphrase this backup was made with.")
                }
            }
            .navigationTitle(confirming ? "Choose a passphrase" : "Open backup")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel", action: onCancel) }
                ToolbarItem(placement: .confirmationAction) {
                    Button(confirming ? "Choose folder" : "Restore") { onContinue(first) }
                        .disabled(problem != nil)
                }
            }
        }
        .presentationDetents([.medium])
    }
}
