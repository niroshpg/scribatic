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
                    .font(.uiBodySmall)
                    .foregroundStyle(Color.inkMuted)
                    .listRowBackground(Color.paper)
                    .listRowInsets(EdgeInsets(top: 0, leading: 0, bottom: 4, trailing: 0))
            }

            Section {
                ForEach(model.models) { row in
                    ModelRowView(row: row) { wanted in
                        if wanted {
                            model.setModelWanted(row.spec, true)
                        } else {
                            confirmLeaveOut = row
                        }
                    }
                    .listRowBackground(Color.surfaceRaised)
                }
            } header: {
                SectionHeader(
                    title: "Models",
                    trailing: "\(ByteCountFormatter.string(fromByteCount: selectedBytes, countStyle: .file)) selected"
                )
            }

            // The fallback, only once the store route has failed.
            if model.packsUnavailable {
                Section {
                    Button {
                        showingManual = true
                    } label: {
                        Label("Install from files", systemImage: "folder")
                    }
                    .listRowBackground(Color.surfaceRaised)
                } header: {
                    SectionHeader(title: "App Store download unavailable")
                } footer: {
                    Text("The models couldn't be downloaded from the App Store on this device. You can download them yourself and install them from files instead.")
                        .font(.uiCaption)
                        .foregroundStyle(Color.inkMuted)
                }
            }
        }
        .listStyle(.insetGrouped)
        .scrollContentBackground(.hidden)
        .background(Color.paper)
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
        .background(Color.paper)
        .safeAreaInset(edge: .bottom) {
            VStack(spacing: 10) {
                if let status = model.packStatus {
                    HStack(spacing: 8) {
                        ProgressView().controlSize(.small).tint(Color.accent)
                        Text(status).font(.uiLabel).foregroundStyle(Color.inkMuted)
                        Spacer()
                    }
                }
                Button {
                    Task { await model.continueFromModels() }
                } label: {
                    Text("Continue").frame(maxWidth: .infinity)
                }
                .buttonStyle(PrimaryButtonStyle())
                .disabled(!model.modelsReady || model.importing != nil)
            }
            .padding(16)
            .frame(maxWidth: 720)
            .frame(maxWidth: .infinity)
            .background(Color.paper)
            .overlay(alignment: .top) { Rectangle().fill(Color.line).frame(height: 1) }
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
        HStack(alignment: .center, spacing: 12) {
            VStack(alignment: .leading, spacing: 2) {
                Text(row.spec.title).font(.uiTitle).foregroundStyle(Color.ink)
                Text(row.spec.purpose).font(.uiBodySmall).foregroundStyle(Color.inkMuted)
                Text("\(row.spec.fileName) · \(ByteCountFormatter.string(fromByteCount: row.spec.sizeBytes, countStyle: .file))")
                    .font(.timestamp)
                    .foregroundStyle(Color.inkSoft)
                status.padding(.top, 2)
            }
            Spacer()
            if row.spec.required {
                Label("Required", systemImage: "lock.fill")
                    .font(.uiLabel)
                    .foregroundStyle(Color.inkMuted)
                    .labelStyle(.titleAndIcon)
            } else {
                Toggle("Use \(row.spec.title)", isOn: Binding(get: { row.wanted }, set: onWantedChange))
                    .labelsHidden()
            }
        }
        .padding(.vertical, 4)
    }

    @ViewBuilder
    private var status: some View {
        if row.installed {
            MetaChip(systemImage: "checkmark.circle.fill", text: "Installed", color: .success)
        } else {
            Text(row.wanted ? "Needed" : "Left out").font(.uiLabel).foregroundStyle(Color.inkMuted)
        }
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

    private var needed: [TranscriptionModel.ModelRow] { model.models.filter(\.wanted) }

    var body: some View {
        List {
            Section {
                Text("Download the model files in Safari, then import them here. Each file is checked before it is used, so its name doesn't matter.")
                    .font(.uiBodySmall)
                    .foregroundStyle(Color.inkMuted)
                    .listRowBackground(Color.paper)
                    .listRowInsets(EdgeInsets(top: 0, leading: 0, bottom: 4, trailing: 0))
            }

            // Numbered because it is a real sequence: download, then import.
            Section {
                StepRow(number: 1, title: "Download model files", help: "Opens the release page in Safari.",
                        systemImage: "safari", tint: .inkMuted) {
                    if let url = ModelSpecValue.downloadPage() { openURL(url) }
                }
                .listRowBackground(Color.surfaceRaised)
                StepRow(number: 2, title: "Import files…", help: "Pick the downloaded files from Files.",
                        systemImage: "square.and.arrow.down", tint: .accentStrong) {
                    picking = true
                }
                .disabled(model.importing != nil)
                .listRowBackground(Color.surfaceRaised)
                if let importing = model.importing {
                    HStack(spacing: 8) {
                        ProgressView().controlSize(.small).tint(Color.accent)
                        Text(importing).font(.uiLabel).foregroundStyle(Color.inkMuted)
                    }
                    .listRowBackground(Color.surfaceRaised)
                }
            } header: {
                SectionHeader(title: "Steps")
            }

            Section {
                ForEach(needed) { row in
                    HStack {
                        Text(row.spec.fileName).font(.timestamp).foregroundStyle(Color.ink)
                        Spacer()
                        if row.installed {
                            MetaChip(systemImage: "checkmark.circle.fill", text: "Installed", color: .success)
                        } else {
                            Text(ByteCountFormatter.string(fromByteCount: row.spec.sizeBytes, countStyle: .file))
                                .font(.timestamp)
                                .foregroundStyle(Color.inkMuted)
                        }
                    }
                    .listRowBackground(Color.surfaceRaised)
                }
            } header: {
                SectionHeader(title: "Needed", trailing: "\(needed.filter(\.installed).count) of \(needed.count) installed")
            }
        }
        .listStyle(.insetGrouped)
        .scrollContentBackground(.hidden)
        .background(Color.paper)
        .navigationTitle("Install from files")
        .navigationBarTitleDisplayMode(.inline)
        .frame(maxWidth: 720)
        .frame(maxWidth: .infinity)
        .background(Color.paper)
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

private struct StepRow: View {
    let number: Int
    let title: String
    let help: String
    let systemImage: String
    let tint: Color
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 14) {
                Text("\(number)")
                    .font(.timestamp)
                    .foregroundStyle(Color.inkMuted)
                    .frame(width: 28, height: 28)
                    .background(Color.fillSecondary, in: Circle())
                VStack(alignment: .leading, spacing: 2) {
                    Text(title).font(.uiTitle).foregroundStyle(Color.ink)
                    Text(help).font(.uiCaption).foregroundStyle(Color.inkMuted)
                }
                Spacer()
                Image(systemName: systemImage).font(.system(size: 18, weight: .medium)).foregroundStyle(tint)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }
}
