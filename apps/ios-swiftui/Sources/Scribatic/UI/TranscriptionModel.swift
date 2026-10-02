import AVFoundation
import BackgroundAssets
import Foundation
import Observation
import UIKit

/// `@MainActor` view model. It never performs work itself — it awaits the
/// `ScribaticEngine` actor and publishes immutable snapshots, so the main actor
/// only ever does diffing and layout.
@MainActor
@Observable
final class TranscriptionModel {

    /// What the recorder is doing, as the UI needs to understand it.
    ///
    /// A single status string conflated two different things — a state to read
    /// calmly and a failure needing action — which left the view unable to tell
    /// them apart, so both rendered as the same grey caption.
    enum Phase: Equatable {
        case starting
        case ready
        case recording
        case paused
        /// After stop: finishing the transcript, saving, identifying speakers.
        case processing(String)
        case failed(String)

        var label: String {
            switch self {
            case .starting:              return "Preparing"
            case .ready:                 return "Ready"
            case .recording:             return "Recording"
            case .paused:                return "Paused"
            case let .processing(step):  return step
            case .failed:                return "Stopped"
            }
        }

        var isFailure: Bool {
            if case .failed = self { return true }
            return false
        }
    }

    /// Screens pushed over the notes list.
    enum Route: Hashable {
        case recorder
        case note(Int64)
    }

    // MARK: - Published state

    private(set) var phase: Phase = .starting
    /// The live transcript of the recording in progress.
    private(set) var segments: [TranscriptSegmentValue] = []
    /// "auto" or the ISO 639-1 code recordings are pinned to.
    private(set) var spokenLanguage = UserDefaults.standard.string(forKey: "language") ?? autoLanguage
    /// The recording's language once known; empty while it is being detected.
    private(set) var sessionLanguage = ""
    private(set) var notes: [NoteSummaryValue] = []
    private(set) var canIdentifySpeakers = false
    /// Which note's recording is playing, if any.
    private(set) var playingNoteID: Int64?
    /// A note-level operation in flight ("Identifying speakers…").
    private(set) var busyNoteID: Int64?
    /// The accurate model is on the device: notes are finished after Stop.
    private(set) var canRefine = false
    /// The note being finished after Stop, what is happening and how far (-1: unknown).
    private(set) var finishingNoteID: Int64?
    private(set) var finishingStep = ""
    private(set) var finishingProgress: Float = -1
    @ObservationIgnored private var finishChain: Task<Void, Never>?
    /// Bumped whenever a note changes, so an open note screen reloads.
    private(set) var noteRevision = 0
    /// Transient failure from a note operation, for an alert.
    var noteError: String?

    var path: [Route] = []

    /// One row of the model setup screen.
    struct ModelRow: Identifiable, Equatable {
        var id: String { spec.id }
        let spec: ModelSpecValue
        let installed: Bool
        let wanted: Bool
    }

    private(set) var models: [ModelRow] = []
    /// First run, or a wanted model went missing: setup replaces everything.
    private(set) var modelsNeeded = false
    /// Setup opened later from the notes list, as a sheet.
    var showingModels = false
    /// "Checking ggml-base.bin — 40%" while an import runs.
    private(set) var importing: String?
    /// "Downloading models — 40%" while Apple-hosted packs download.
    private(set) var packStatus: String?
    /// Packs could not be fetched from Apple: offer importing instead.
    private(set) var packsUnavailable = false
    /// The pack download under way, so switching its model off can stop it.
    @ObservationIgnored private var packDownload: (id: String, task: Task<Void, Error>)?

    /// The required models are present, so the engine can start. An optional
    /// model that is still on its way does not hold the app back: it finishes
    /// in the background and is picked up the next time the engine starts.
    var modelsReady: Bool {
        !models.isEmpty && models.filter(\.spec.required).allSatisfy(\.installed)
    }

    private let installer = ModelInstaller()

    var failureMessage: String? {
        if case let .failed(message) = phase { return message }
        return nil
    }

    private var engine: ScribaticEngine?
    private var configuration: ScribaticEngine.Configuration?
    private var streamTask: Task<Void, Never>?
    private let capture = AudioCapture()
    private var recordingStartedAt = Date()
    private var player: AVAudioPlayer?
    private var playerDelegate: PlayerDelegate?

    // MARK: - Lifecycle

    /// Warms the engine only. The microphone is deliberately NOT opened here:
    /// for an app whose whole claim is that audio never leaves the device, a
    /// mic that goes live the instant the app launches is the wrong default.
    /// Capture begins when the user presses record, and not before.
    func prepare() async {
        guard engine == nil else { return }
        phase = .starting

        installer.sweepPartials()
        refreshModels()
        guard installer.isReady() else {
            modelsNeeded = true
            Task { await fetchWantedPacks() }
            return
        }

        do {
            let configuration = try ScribaticEngine.Configuration.resolved(installer)
            let engine = try ScribaticEngine(configuration: configuration)
            self.configuration = configuration
            self.engine = engine
            try await engine.warmUp()
            canIdentifySpeakers = engine.canIdentifySpeakers
            canRefine = engine.canRefine
            await refreshNotes()
            phase = .ready
        } catch {
            // Say which model and why, not only the engine's status: this is
            // what a tester copies into a report.
            phase = .failed("\(error)\n\n\(installer.diagnosis())")
            // Otherwise the `engine == nil` guard makes "Try again" a no-op.
            self.engine = nil
        }
    }

    // MARK: - Models

    /// Optional models an installed add-on uses, by file name; the others are
    /// not offered, since nothing would use them. iOS has no add-ons yet, so
    /// none: the summaries model would be a 1.2 GB download that does nothing.
    static var optionalModelsInUse: Set<String> = []

    func refreshModels() {
        models = installer.catalog
            .filter { $0.required || Self.optionalModelsInUse.contains($0.fileName) }
            .map { ModelRow(spec: $0, installed: installer.isInstalled($0), wanted: installer.isWanted($0)) }
    }

    /// Leaving out an optional model also deletes it if installed: the only
    /// reason to decline the instruct model is its 1.2 GB.
    func setModelWanted(_ spec: ModelSpecValue, _ wanted: Bool) {
        installer.setWanted(spec, wanted)
        refreshModels()
        let pack = ModelInstaller.packID(for: spec)
        Task {
            if wanted {
                await fetchWantedPacks()
            } else {
                // Removing alone leaves a download that is under way running.
                if let download = packDownload, download.id == pack { download.task.cancel() }
                await installer.removePack(pack)
                refreshModels()
            }
        }
    }

    /// Fetches every wanted pack not yet on the device from Apple, reporting
    /// progress. If this install cannot get packs at all, flags it so the
    /// setup screen leads with importing.
    func fetchWantedPacks() async {
        let missing = Set(models.filter { $0.wanted && !$0.installed }.map { ModelInstaller.packID(for: $0.spec) })
        guard !missing.isEmpty, packStatus == nil else { return }

        // Required pack first: the app can start as soon as it lands, and the
        // 1.2 GB optional one must not keep it waiting.
        for id in missing.sorted(by: { a, _ in a == "models-core" }) {
            let progress = Task { [weak self] in
                for await update in AssetPackManager.shared.statusUpdates(forAssetPackWithID: id) {
                    if case let .downloading(_, fraction) = update {
                        let percent = Int(fraction.fractionCompleted * 100)
                        await MainActor.run { self?.packStatus = "Downloading models — \(percent)%" }
                    }
                }
            }
            packStatus = "Downloading models"
            let installer = self.installer
            let download = Task { try await installer.fetchPack(id) }
            packDownload = (id, download)
            do {
                try await download.value
            } catch {
                // A cancel is the user leaving the model out, not a sign that
                // this install can't get packs.
                if !download.isCancelled { packsUnavailable = true }
            }
            packDownload = nil
            progress.cancel()
            refreshModels()
            // First run, blocked only on the required pack: carry on the
            // moment it lands, without waiting for the optional one.
            if engine == nil, modelsNeeded, modelsReady { await continueFromModels() }
        }
        packStatus = nil
        refreshModels()
        // First run, blocked only on the download: carry on.
        if engine == nil, modelsNeeded, modelsReady { await continueFromModels() }
    }

    func importModels(_ urls: [URL]) async {
        guard importing == nil, !urls.isEmpty else { return }
        let installer = self.installer
        var problems: [String] = []
        for url in urls {
            let outcome = await Task.detached(priority: .userInitiated) {
                installer.importFile(url) { name, fraction in
                    Task { @MainActor [weak self] in
                        self?.importing = "Checking \(name) — \(Int(fraction * 100))%"
                    }
                }
            }.value
            switch outcome {
            case let .notAModel(name): problems.append("\(name) is not one of the model files.")
            case let .failed(name, reason): problems.append("\(name): \(reason)")
            case .installed, .alreadyInstalled: break
            }
        }
        importing = nil
        refreshModels()
        if !problems.isEmpty { noteError = problems.joined(separator: "\n") }
    }

    /// From the setup screen: start the engine, or just close if it runs.
    func continueFromModels() async {
        guard modelsReady else { return }
        modelsNeeded = false
        showingModels = false
        await prepare()
    }

    func refreshNotes() async {
        guard let engine else { return }
        notes = await engine.listNotes()
    }

    func setSpokenLanguage(_ code: String) {
        UserDefaults.standard.set(code, forKey: "language")
        spokenLanguage = code
    }

    // MARK: - Recording

    func startRecording() async {
        guard let engine, let configuration else { return }
        stopPlayback()

        do {
            try await AudioCapture.requestPermission()
            await engine.setLanguage(spokenLanguage)
            await engine.beginSession()
            segments = []
            sessionLanguage = ""
            recordingStartedAt = Date()
            let url = AudioCapture.newRecordingURL(in: URL(filePath: configuration.recordingsDirectory))
            try capture.start(writingTo: url) { buffer, frameCount in
                // Render thread. nonisolated by design — the lock-free ring
                // buffer on the C++ side is what makes this safe.
                engine.pushAudio(buffer, frameCount: frameCount)
            }
            phase = .recording
            startStreaming()
        } catch {
            phase = .failed(error.localizedDescription)
        }
    }

    func pauseRecording() {
        guard phase == .recording else { return }
        capture.pause()
        phase = .paused
    }

    func resumeRecording() {
        guard phase == .paused else { return }
        do {
            try capture.resume()
            phase = .recording
        } catch {
            phase = .failed(error.localizedDescription)
        }
    }

    /// Stops capture, then turns the recording into a saved note: decode the
    /// tail, persist, identify speakers, and open the note.
    ///
    /// Speakers are identified here, straight away, because it needs the
    /// audio — and deleting the audio is the very next thing a user may do.
    func stopRecording() {
        guard phase == .recording || phase == .paused else { return }
        capture.stop()
        streamTask?.cancel()
        streamTask = nil
        let audioURL = capture.recordingURL
        let startedAt = recordingStartedAt

        phase = .processing("Finishing transcript")
        Task { [weak self] in
            guard let self, let engine = self.engine else { return }
            // The decode window is five seconds, so without the flush
            // everything said since the last boundary would be lost.
            if let tail = try? await engine.flush(), !tail.isEmpty {
                self.segments.append(contentsOf: tail)
            }

            self.phase = .processing("Saving")
            let title = startedAt.formatted(date: .abbreviated, time: .shortened)
            guard let id = try? await engine.saveSession(title: title, createdAt: startedAt, audioURL: audioURL) else {
                self.phase = .failed("The recording could not be saved.")
                return
            }

            await self.refreshNotes()
            self.segments = []
            self.phase = .ready
            // Replace the recorder with the note it produced: the live
            // preview now, the final transcript once it is finished.
            self.path = [.note(id)]
            self.finishNote(id)
        }
    }

    /// The final transcript: the whole recording again with the accurate
    /// model, then speakers from its words. Queued behind any note still
    /// finishing, so a new recording can start meanwhile.
    func finishNote(_ id: Int64) {
        let previous = finishChain
        finishChain = Task { [weak self] in
            await previous?.value
            guard let self, let engine = self.engine else { return }
            guard let note = await engine.loadNote(id), note.audioURL != nil else { return }
            // Time to finish if the user leaves the app; iOS may still stop
            // it, and the note keeps its preview with "Improve transcript".
            let background = UIApplication.shared.beginBackgroundTask(withName: "Finish transcript")
            defer {
                if background != .invalid { UIApplication.shared.endBackgroundTask(background) }
            }
            if self.canRefine {
                self.finishing(id, "Improving transcript", 0)
                let poll = Task { [weak self] in
                    while !Task.isCancelled {
                        try? await Task.sleep(for: .milliseconds(400))
                        guard let self, self.finishingNoteID == id else { continue }
                        self.finishingProgress = engine.refineProgress
                    }
                }
                do {
                    try await engine.refineTranscript(id)
                } catch {
                    self.noteError = "The transcript could not be improved: \(error)"
                }
                poll.cancel()
                await self.noteChanged()
            }
            if self.canIdentifySpeakers {
                self.finishing(id, "Identifying speakers", -1)
                do {
                    try await engine.identifySpeakers(id)
                } catch {
                    // The transcript is saved either way; speakers can be
                    // identified again from the note while the audio exists.
                    self.noteError = "Speakers could not be identified: \(error)"
                }
            }
            self.finishingNoteID = nil
            self.finishingStep = ""
            await self.refreshNotes()
            await self.noteChanged()
        }
    }

    private func finishing(_ id: Int64, _ step: String, _ progress: Float) {
        finishingNoteID = id
        finishingStep = step
        finishingProgress = progress
    }

    // MARK: - Correcting speakers

    /// Every segment of a block to `speaker`; a negative one makes a new speaker.
    func setBlockSpeaker(_ id: Int64, segments: [Int64], speaker: Int32) async {
        guard let engine, let first = segments.first else { return }
        do {
            var target = speaker
            if target < 0 {
                try await engine.setSegmentSpeaker(id, segment: first, speaker: -1)
                guard let landed = await engine.loadNote(id)?.segments.first(where: { $0.segmentId == first })?.speaker else { return }
                target = landed
            }
            for segment in segments where !(speaker < 0 && segment == first) {
                try await engine.setSegmentSpeaker(id, segment: segment, speaker: target)
            }
        } catch {
            noteError = "Could not change the speaker: \(error)"
        }
        await noteChanged()
    }

    func mergeSpeakers(_ id: Int64, speakers: [Int32], into: Int32) async {
        guard let engine else { return }
        do {
            try await engine.mergeSpeakers(id, speakers: speakers, into: into)
        } catch {
            noteError = "Could not merge speakers: \(error)"
        }
        await noteChanged()
    }

    func setLayout(_ id: Int64, layout: String) async {
        guard let engine else { return }
        do {
            try await engine.setNoteLayout(id, layout: layout)
        } catch {
            noteError = "Could not change the layout: \(error)"
        }
        await noteChanged()
    }

    // MARK: - Notes

    func loadNote(_ id: Int64) async -> NoteDetailValue? {
        await engine?.loadNote(id)
    }

    func identifySpeakers(_ id: Int64, expected: Int32) async {
        guard let engine else { return }
        if playingNoteID == id { stopPlayback() }
        busyNoteID = id
        defer { busyNoteID = nil }
        do {
            try await engine.identifySpeakers(id, expected: expected)
        } catch {
            noteError = "Speakers could not be identified: \(error)"
        }
        await noteChanged()
    }

    func renameSpeaker(_ id: Int64, speaker: Int32, name: String) async {
        guard let engine else { return }
        do {
            let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
            try await engine.renameSpeaker(id, speaker: speaker, name: trimmed)
        } catch {
            noteError = "\(error)"
        }
        await noteChanged()
    }

    /// Removes the audio and keeps the transcript.
    func deleteRecording(_ id: Int64) async {
        guard let engine else { return }
        if playingNoteID == id { stopPlayback() }
        do {
            try await engine.deleteRecording(id)
        } catch {
            noteError = "\(error)"
        }
        await noteChanged()
    }

    func deleteNote(_ id: Int64) async {
        guard let engine else { return }
        if playingNoteID == id { stopPlayback() }
        do {
            try await engine.deleteNote(id)
        } catch {
            noteError = "\(error)"
        }
        path.removeAll { $0 == .note(id) }
        await noteChanged()
    }

    func exportTranscript(_ id: Int64, anonymise: Bool) async -> String {
        await engine?.exportTranscript(id, includeTimestamps: true, anonymise: anonymise) ?? ""
    }

    private func noteChanged() async {
        noteRevision += 1
        await refreshNotes()
    }

    // MARK: - Playback

    func play(_ note: NoteDetailValue) {
        guard let url = note.audioURL else { return }
        stopPlayback()

        do {
            let player = try AVAudioPlayer(contentsOf: url)
            let delegate = PlayerDelegate { [weak self] in
                Task { @MainActor in self?.playingNoteID = nil }
            }
            player.delegate = delegate
            self.playerDelegate = delegate
            self.player = player
            player.play()
            playingNoteID = note.id
        } catch {
            noteError = error.localizedDescription
        }
    }

    func stopPlayback() {
        player?.stop()
        player = nil
        playerDelegate = nil
        playingNoteID = nil
    }

    // MARK: - Transcript stream

    private func startStreaming() {
        streamTask?.cancel()
        streamTask = Task { [weak self] in
            guard let self, let engine = await self.engine else { return }
            do {
                for try await batch in await engine.transcriptionStream() {
                    let language = await engine.sessionLanguage()
                    await MainActor.run {
                        self.segments.append(contentsOf: batch)
                        self.sessionLanguage = language
                    }
                }
            } catch is CancellationError {
                // Expected on stop.
            } catch {
                await MainActor.run { self.phase = .failed("\(error)") }
            }
        }
    }

    func hibernate() async {
        stopRecording()
        stopPlayback()
        streamTask?.cancel()
        await engine?.hibernate()
    }
}

/// AVAudioPlayer's delegate is an Objective-C protocol, so it cannot be the
/// @MainActor @Observable model itself without dragging isolation into a
/// callback that arrives on an arbitrary queue.
private final class PlayerDelegate: NSObject, AVAudioPlayerDelegate {
    private let onFinish: () -> Void

    init(onFinish: @escaping () -> Void) {
        self.onFinish = onFinish
    }

    func audioPlayerDidFinishPlaying(_ player: AVAudioPlayer, successfully flag: Bool) {
        onFinish()
    }
}
