import AVFoundation
import Foundation
import Observation

/// `@MainActor` view model. It never performs work itself — it awaits the
/// `ScribaticEngine` actor and publishes immutable snapshots, so the main actor
/// only ever does diffing and layout.
@MainActor
@Observable
final class TranscriptionModel {

    /// What the app is doing, as the UI needs to understand it.
    ///
    /// A single status string conflated two different things — a state to read
    /// calmly and a failure needing action — which left the view unable to tell
    /// them apart, so both rendered as the same grey caption.
    enum Phase: Equatable {
        case starting
        case ready
        case recording
        case paused
        case playing
        case failed(String)

        var label: String {
            switch self {
            case .starting:  return "Preparing"
            case .ready:     return "Ready"
            case .recording: return "Recording"
            case .paused:    return "Paused"
            case .playing:   return "Playing"
            case .failed:    return "Stopped"
            }
        }

        var isFailure: Bool {
            if case .failed = self { return true }
            return false
        }
    }

    private(set) var segments: [TranscriptSegmentValue] = []
    private(set) var summary: String?
    private(set) var phase: Phase = .starting

    /// True once a capture has been made and not cleared, which is what makes
    /// playback and clearing meaningful.
    private(set) var hasRecording = false

    /// Clearing is only meaningful when there is something to discard.
    var canClear: Bool {
        !segments.isEmpty || hasRecording
    }

    var failureMessage: String? {
        if case let .failed(message) = phase { return message }
        return nil
    }

    private var engine: ScribaticEngine?
    private var streamTask: Task<Void, Never>?
    private let capture = AudioCapture()
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

        do {
            let engine = try ScribaticEngine(configuration: .default())
            self.engine = engine
            try await engine.warmUp()
            phase = .ready
        } catch {
            phase = .failed("\(error)")
        }
    }

    // MARK: - Transport

    func startRecording() async {
        guard let engine else { return }
        stopPlayback()

        do {
            try await AudioCapture.requestPermission()
            try capture.start { buffer, frameCount in
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

    func stopRecording() {
        guard phase == .recording || phase == .paused else { return }
        capture.stop()
        streamTask?.cancel()
        streamTask = nil
        hasRecording = capture.recordingURL != nil
        phase = .ready
    }

    /// Discards the transcript and the captured audio. Deleting the file rather
    /// than orphaning it matters here: an audio file that outlives the note it
    /// belongs to is the same class of defect as a vector index that does.
    func clear() {
        stopPlayback()
        segments.removeAll()
        summary = nil

        if let url = capture.recordingURL {
            try? FileManager.default.removeItem(at: url)
        }
        hasRecording = false
        if !phase.isFailure { phase = .ready }
    }

    // MARK: - Playback

    func play() {
        guard hasRecording, let url = capture.recordingURL else { return }

        do {
            let player = try AVAudioPlayer(contentsOf: url)
            let delegate = PlayerDelegate { [weak self] in
                Task { @MainActor in
                    guard let self, self.phase == .playing else { return }
                    self.phase = .ready
                }
            }
            player.delegate = delegate
            self.playerDelegate = delegate
            self.player = player
            player.play()
            phase = .playing
        } catch {
            phase = .failed(error.localizedDescription)
        }
    }

    func stopPlayback() {
        player?.stop()
        player = nil
        playerDelegate = nil
        if phase == .playing { phase = .ready }
    }

    // MARK: - Transcript stream

    private func startStreaming() {
        streamTask?.cancel()
        streamTask = Task { [weak self] in
            guard let self, let engine = await self.engine else { return }
            do {
                for try await batch in await engine.transcriptionStream() {
                    await MainActor.run { self.segments.append(contentsOf: batch) }
                }
            } catch is CancellationError {
                // Expected on stop.
            } catch {
                await MainActor.run { self.phase = .failed("\(error)") }
            }
        }
    }

    func summarizeTranscript() async {
        guard let engine else { return }
        let transcript = segments.map(\.text).joined(separator: " ")
        summary = try? await engine.summarize(transcript: transcript)
    }

    func hibernate() async {
        stopRecording()
        stopPlayback()
        streamTask?.cancel()
        await engine?.hibernate()
        phase = .ready
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
