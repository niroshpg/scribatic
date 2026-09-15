import Foundation
import Observation

/// `@MainActor` view model. It never performs work itself — it awaits the
/// `ScribaticEngine` actor and publishes immutable snapshots, so the main actor
/// only ever does diffing and layout.
@MainActor
@Observable
final class TranscriptionModel {

    /// What the engine is doing, as the UI needs to understand it.
    ///
    /// A single status string conflated two different things — a state the user
    /// should read calmly ("Listening") and a failure they need to act on
    /// ("model file not found") — which left the view unable to tell them
    /// apart, so both rendered as the same grey caption.
    enum Phase: Equatable {
        case idle
        case starting
        case listening
        case transcribing
        case failed(String)

        var label: String {
            switch self {
            case .idle:            return "Idle"
            case .starting:        return "Starting"
            case .listening:       return "Listening"
            case .transcribing:    return "Transcribing"
            case .failed:          return "Stopped"
            }
        }

        var isFailure: Bool {
            if case .failed = self { return true }
            return false
        }
    }

    private(set) var segments: [TranscriptSegmentValue] = []
    private(set) var summary: String?
    private(set) var phase: Phase = .idle

    /// The failure text, when there is one. Drives the error presentation.
    var failureMessage: String? {
        if case let .failed(message) = phase { return message }
        return nil
    }

    private var engine: ScribaticEngine?
    private var streamTask: Task<Void, Never>?

    func start() async {
        guard engine == nil else { return }
        phase = .starting

        do {
            let engine = try ScribaticEngine(configuration: .default())
            self.engine = engine
            try await engine.warmUp()
            phase = .listening

            streamTask = Task { [weak self] in
                guard let self, let engine = await self.engine else { return }
                do {
                    for try await batch in await engine.transcriptionStream() {
                        await MainActor.run {
                            self.segments.append(contentsOf: batch)
                            self.phase = .listening
                        }
                    }
                } catch {
                    await MainActor.run { self.phase = .failed("\(error)") }
                }
            }
        } catch {
            phase = .failed("\(error)")
        }
    }

    func summarizeTranscript() async {
        guard let engine else { return }
        let transcript = segments.map(\.text).joined(separator: " ")
        summary = try? await engine.summarize(transcript: transcript)
    }

    func hibernate() async {
        streamTask?.cancel()
        await engine?.hibernate()
        phase = .idle
    }
}
