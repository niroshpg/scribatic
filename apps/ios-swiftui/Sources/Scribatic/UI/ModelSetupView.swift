import SwiftUI
import UniformTypeIdentifiers

/// The models the app runs on, where to get them, and importing them. Shown
/// on first run in place of everything else, and later from the notes list.
struct ModelSetupView: View {
    @Bindable var model: TranscriptionModel
    @Environment(\.openURL) private var openURL
    @State private var picking = false
    @State private var confirmLeaveOut: TranscriptionModel.ModelRow?

    private var selectedBytes: Int64 {
        model.models.filter(\.wanted).map(\.spec.sizeBytes).reduce(0, +)
    }

    var body: some View {
        List {
            Section {
                Text("Scribatic runs entirely on this device, so it needs these model files. The app never connects to the internet: download them in Safari, then import them here.")
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

            Section {
                if let status = model.packStatus {
                    HStack {
                        ProgressView()
                        Text(status).font(.subheadline)
                    }
                }
                if let importing = model.importing {
                    HStack {
                        ProgressView()
                        Text(importing).font(.subheadline)
                    }
                }
                Button("1. Download model files", systemImage: "safari") {
                    if let url = ModelSpecValue.downloadPage() { openURL(url) }
                }
                Button("2. Import files…", systemImage: "square.and.arrow.down") { picking = true }
                    .disabled(model.importing != nil)
            }
        }
        .navigationTitle("Models")
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
        .fileImporter(isPresented: $picking, allowedContentTypes: [.data], allowsMultipleSelection: true) { result in
            if case let .success(urls) = result {
                Task { await model.importModels(urls) }
            }
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
