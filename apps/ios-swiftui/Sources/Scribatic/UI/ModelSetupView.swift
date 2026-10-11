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
    @State private var speakers = SpeakerIdentification.current

    private var selectedBytes: Int64 {
        model.models.filter(\.wanted).map(\.bytes).reduce(0, +)
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
                Picker(selection: Binding(get: { model.spokenLanguage }, set: { model.setSpokenLanguage($0) })) {
                    Text(languageName(autoLanguage)).tag(autoLanguage)
                    ForEach(spokenLanguages.sorted { languageName($0) < languageName($1) }, id: \.self) { code in
                        Text(code == "en" ? languageName(code) : "\(languageName(code)) · experimental").tag(code)
                    }
                } label: {
                    Label {
                        VStack(alignment: .leading, spacing: 2) {
                            Text("Spoken language").font(.uiBody).foregroundStyle(Color.ink)
                            Text(model.spokenLanguage == autoLanguage
                                 ? "Each recording's language is worked out from its first words. English is supported; other languages are experimental."
                                 : model.spokenLanguage == "en"
                                 ? "Every recording is transcribed as English."
                                 : "Every recording is transcribed as \(languageName(model.spokenLanguage)). Experimental: results may be poor.")
                                .font(.uiBodySmall)
                                .foregroundStyle(Color.inkMuted)
                        }
                    } icon: {
                        Image(systemName: "character.bubble").foregroundStyle(Color.inkMuted)
                    }
                }
                .pickerStyle(.navigationLink)
                .listRowBackground(Color.surfaceRaised)
                Picker("Identify speakers", selection: $speakers) {
                    ForEach(SpeakerIdentification.allCases) { Text($0.label).tag($0) }
                }
                .font(.uiBody)
                .listRowBackground(Color.surfaceRaised)
                .onChange(of: speakers) { _, new in SpeakerIdentification.current = new }
            } header: {
                SectionHeader(title: "Transcription")
            } footer: {
                Text(speakers.explanation)
                    .font(.uiCaption)
                    .foregroundStyle(Color.inkMuted)
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

            StorageSection(model: model)
            if model.canBackUp {
                BackupSection(model: model)
            }

            Section {
                NavigationLink("Open-source licences") { LicencesView() }
                    .font(.uiBody)
                    .foregroundStyle(Color.ink)
                    .listRowBackground(Color.surfaceRaised)
            } header: {
                SectionHeader(title: "About")
            }

            // The models come only from the App Store: nothing else hosts them.
            if model.packsUnavailable {
                Section {
                    Button("Try again", systemImage: "arrow.clockwise") { model.retryPacks() }
                        .listRowBackground(Color.surfaceRaised)
                } header: {
                    SectionHeader(title: "App Store download unavailable")
                } footer: {
                    Text("The models couldn't be downloaded from the App Store. Check the connection, then try again.")
                        .font(.uiCaption)
                        .foregroundStyle(Color.inkMuted)
                }
            }
        }
        .listStyle(.insetGrouped)
        .scrollContentBackground(.hidden)
        .background(Color.paper)
        .overlay {
            if let progress = model.backupProgress {
                ProgressView(progress)
                    .font(.uiLabel)
                    .padding(24)
                    .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
            }
        }
        // Settings once running (models, storage, backup); only the models
        // while they are first being set up.
        .navigationTitle(model.canBackUp ? "Settings" : "Models")
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
                .disabled(!model.modelsReady)
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
                ? " Its \(ByteCountFormatter.string(fromByteCount: row.bytes, countStyle: .file)) is removed from this device."
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
                Text("\(row.files.joined(separator: " + ")) · \(ByteCountFormatter.string(fromByteCount: row.bytes, countStyle: .file))")
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
struct StorageSection: View {
    let model: TranscriptionModel
    @State private var status: DeviceStatus?
    @State private var level = DeviceStatus.warningLevel
    @State private var keep = KeepRecordings.current

    var body: some View {
        Section {
            if let status {
                row("Free on this device", bytes(status.freeBytes),
                    detail: "\(durationEstimate(status.recordingTimeLeft)) of recording")
                row("Recordings", bytes(status.recordingsBytes),
                    detail: status.recordingCount == 1 ? "1 recording" : "\(status.recordingCount) recordings")
                row("Models", bytes(status.modelsBytes))
                row("Notes", bytes(status.notesBytes))
                row("Memory available to Scribatic", bytes(status.availableMemory),
                    detail: model.lowMemory ? "iOS reported low memory recently"
                                            : "of \(bytes(status.totalMemory)) on this device",
                    warning: model.lowMemory)
            }
            Picker("Warn before recording below", selection: $level) {
                ForEach(DeviceStatus.warningLevels, id: \.self) { Text(bytes($0)).tag($0) }
            }
            .font(.uiBody)
            .listRowBackground(Color.surfaceRaised)
            .onChange(of: level) { _, new in DeviceStatus.warningLevel = new }
            Picker("Keep recordings", selection: $keep) {
                ForEach(KeepRecordings.allCases) { Text($0.label).tag($0) }
            }
            .font(.uiBody)
            .listRowBackground(Color.surfaceRaised)
            .onChange(of: keep) { _, new in
                KeepRecordings.current = new
                Task {
                    await model.settleAllRecordings()
                    status = model.deviceStatus()
                }
            }
        } header: {
            SectionHeader(title: "Storage & memory")
        } footer: {
            Text("\(keep.explanation) While recording, about 230 MB an hour is needed until the transcript is finished. If storage runs out while recording, it stops and what was recorded is saved.")
                .font(.uiCaption)
                .foregroundStyle(Color.inkMuted)
        }
        .task { status = model.deviceStatus() }
    }

    private func row(_ title: String, _ value: String, detail: String? = nil, warning: Bool = false) -> some View {
        HStack(alignment: .firstTextBaseline) {
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.uiBody).foregroundStyle(Color.ink)
                if let detail {
                    Text(detail).font(.uiBodySmall).foregroundStyle(warning ? Color.danger : Color.inkMuted)
                }
            }
            Spacer()
            Text(value).font(.timestamp).foregroundStyle(Color.inkMuted)
        }
        .listRowBackground(Color.surfaceRaised)
        .accessibilityElement(children: .combine)
    }

    private func bytes(_ count: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: count, countStyle: .file)
    }
}


/// The notices for the software and models the app is built on
/// (licenses/ at the repository root, copied into the bundle).
struct LicencesView: View {
    // Paragraphs, so the 380 KB of text is laid out only as it scrolls into view.
    private let paragraphs: [String] = {
        guard let url = Bundle.main.url(forResource: "THIRD_PARTY_NOTICES", withExtension: "txt"),
              let text = try? String(contentsOf: url, encoding: .utf8) else { return [] }
        return text.components(separatedBy: "\n\n")
    }()

    var body: some View {
        ScrollView {
            LazyVStack(alignment: .leading, spacing: 8) {
                ForEach(paragraphs.indices, id: \.self) { index in
                    Text(paragraphs[index])
                        .font(.uiCaption)
                        .foregroundStyle(Color.ink)
                        .textSelection(.enabled)
                }
            }
            .padding(16)
        }
        .background(Color.paper)
        .navigationTitle("Open-source licences")
        .navigationBarTitleDisplayMode(.inline)
    }
}
