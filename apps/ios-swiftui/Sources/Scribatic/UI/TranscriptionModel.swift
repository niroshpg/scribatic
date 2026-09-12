import Foundation
import Observation

/// `@MainActor` view model. It never performs work itself — it awaits the
/// `ScribaticEngine` actor and publishes immutable snapshots, so the main actor
/// only ever does diffing and layout.
@MainActor
@Observable
final class TranscriptionModel {
    private(set) var segments: [TranscriptSegmentValue] = []
    private(set) var summary: String?
    private(set) var statusLabel: String = "Idle"

    private var engine: ScribaticEngine?
    private var streamTask: Task<Void, Never>?

    func start() async {
        do {
            let engine = try ScribaticEngine(configuration: .default())
            self.engine = engine
            try await engine.warmUp()
            statusLabel = "Listening"

            streamTask = Task { [weak self] in
                guard let self, let engine = await self.engine else { return }
                do {
                    for try await batch in await engine.transcriptionStream() {
                        await MainActor.run { self.segments.append(contentsOf: batch) }
                    }
                } catch {
                    await MainActor.run { self.statusLabel = "\(error)" }
                }
            }
        } catch {
            statusLabel = "\(error)"
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
        statusLabel = "Idle"
    }
}
