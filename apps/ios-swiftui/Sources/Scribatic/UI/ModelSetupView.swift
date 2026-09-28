import SwiftUI
import UniformTypeIdentifiers

/// The models the app runs on, and their App Store download. Shown on first
/// run in place of everything else, and later from the notes list.
///
/// Installing from files is deliberately NOT on this screen: next to a
/// download already in progress, a second way to get the same files reads as
/// a choice the user has to make. It lives on its own screen, offered here
/// only when the App Store can't provide the models, or chosen from the menu.
struct ModelSetupView: View {
    @Bindable var model: TranscriptionModel
    @State private var confirmLeaveOut: TranscriptionModel.ModelRow?
    @State private var showingManual = false

    private var selectedBytes: Int64 {
        model.models.filter(\.wanted).map(\.spec.sizeBytes).reduce(0, +)
    }

    var body: some View {
        List {
            Section {
                Text("Scribatic runs entirely on this device, so it needs these models. The App Store downloads them for you; the app itself never connects to the internet.")
                    .font(.subheadline)
            } footer: {
                Text("Selected: \(ByteCountFormatter.string(fromByteCount: selectedBytes, countStyle: .file))")
            }

            Section("Models") {
                ForEach(model.models) { row in
                    ModelRowView(row: row) { wanted in
                        if wanted {
                            model.setModelWanted(row.spec, true)
                        } else {
                            confirmLeaveOut = row
                        }
                    }
                }
            }

            if let status = model.packStatus {
                Section {
                    HStack {
                        ProgressView()
                        Text(status).font(.subheadline)
                    }
                }
            }

            // The fallback, only once the store route has failed.
            if model.packsUnavailable {
                Section {
                    Button("Install from files", systemImage: "folder") { showingManual = true }
                } header: {
                    Text("App Store download unavailable")
                } footer: {
                    Text("The models couldn't be downloaded from the App Store on this device. You can download them yourself and install them from files instead.")
                }
            }
        }
        .navigationTitle("Models")
        .toolbar {
            ToolbarItem(placement: .secondaryAction) {
                Button("Install from files…", systemImage: "folder") { showingManual = true }
            }
        }
        .navigationDestination(isPresented: $showingManual) {
            ModelFilesView(model: model)
        }
        .frame(maxWidth: 720)
        .frame(maxWidth: .infinity)
        .safeAreaInset(edge: .bottom) {
            Button {
                Task { await model.continueFromModels() }
            } label: {
                Text("Continue").font(.headline).frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.large)
            .tint(.scribaticAccent)
            .disabled(!model.modelsReady || model.importing != nil)
            .padding()
            .background(.bar)
        }
        .alert("Leave out \(confirmLeaveOut?.spec.title.lowercased() ?? "")?",
               isPresented: Binding(get: { confirmLeaveOut != nil }, set: { if !$0 { confirmLeaveOut = nil } }),
               presenting: confirmLeaveOut) { row in
            Button("Leave it out", role: .destructive) { model.setModelWanted(row.spec, false) }
            Button("Keep it", role: .cancel) {}
        } message: { row in
            Text(row.spec.withoutIt + (row.installed
                ? " Its \(ByteCountFormatter.string(fromByteCount: row.spec.sizeBytes, countStyle: .file)) is removed from this device."
                : ""))
        }
        .onAppear { model.refreshModels() }
    }
}

private struct ModelRowView: View {
    let row: TranscriptionModel.ModelRow
    let onWantedChange: (Bool) -> Void

    var body: some View {
        HStack(alignment: .top) {
            VStack(alignment: .leading, spacing: 2) {
                Text(row.spec.title).font(.headline)
                Text(row.spec.purpose).font(.subheadline).foregroundStyle(.secondary)
                Text("\(row.spec.fileName) · \(ByteCountFormatter.string(fromByteCount: row.spec.sizeBytes, countStyle: .file)) · \(status)")
                    .font(.caption)
                    .foregroundStyle(row.installed ? Color.green : Color.secondary)
            }
            Spacer()
            if row.spec.required {
                Text("Required").font(.caption).foregroundStyle(.secondary)
            } else {
                Toggle("Use \(row.spec.title)", isOn: Binding(get: { row.wanted }, set: onWantedChange))
                    .labelsHidden()
            }
        }
        .padding(.vertical, 2)
    }

    private var status: String {
        if row.installed { return "Installed" }
        return row.wanted ? "Needed" : "Left out"
    }
}

/// Installing the models from files: download them in Safari from the
/// project's release page, then import them. A separate screen from the App
/// Store download on purpose — see `ModelSetupView`.
struct ModelFilesView: View {
    @Bindable var model: TranscriptionModel
    @Environment(\.openURL) private var openURL
    @Environment(\.dismiss) private var dismiss
    @State private var picking = false

    var body: some View {
        List {
            Section {
                Text("Download the model files in Safari, then import them here. Each file is checked before it is used, so its name doesn't matter.")
                    .font(.subheadline)
            }

            Section("Needed") {
                ForEach(model.models.filter(\.wanted)) { row in
                    LabeledContent(row.spec.fileName) {
                        Text(row.installed
                             ? "Installed"
                             : ByteCountFormatter.string(fromByteCount: row.spec.sizeBytes, countStyle: .file))
                            .foregroundStyle(row.installed ? Color.green : Color.secondary)
                    }
                    .font(.subheadline)
                }
            }

            Section {
                Button("1. Download model files", systemImage: "safari") {
                    if let url = ModelSpecValue.downloadPage() { openURL(url) }
                }
                Button("2. Import files…", systemImage: "square.and.arrow.down") { picking = true }
                    .disabled(model.importing != nil)
                if let importing = model.importing {
                    HStack {
                        ProgressView()
                        Text(importing).font(.subheadline)
                    }
                }
            }
        }
        .navigationTitle("Install from files")
        .navigationBarTitleDisplayMode(.inline)
        .frame(maxWidth: 720)
        .frame(maxWidth: .infinity)
        .fileImporter(isPresented: $picking, allowedContentTypes: [.data], allowsMultipleSelection: true) { result in
            if case let .success(urls) = result {
                Task {
                    await model.importModels(urls)
                    // Everything needed is in: back to the setup screen, whose
                    // Continue is now enabled.
                    if model.modelsReady { dismiss() }
                }
            }
        }
    }
}
