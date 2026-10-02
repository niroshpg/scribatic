import Foundation
import ScribaticCore   // C++ module, imported directly — no Objective-C++ layer

/// Swift-side owner of the shared C++ engine.
///
/// ## Why an actor
/// Every inference entry point is `async` and runs on this actor's serial
/// executor, which is a cooperative-pool thread — never the main actor. A
/// whisper encode holds a core for 200–800 ms; performing it on the main actor
/// would stall the display link and drop frames on a 120 Hz ProMotion panel
/// long before it produced a watchdog termination.
///
/// ## Why `nonisolated` audio ingress
/// `pushAudio` is `nonisolated` and synchronous because it is called from the
/// `AVAudioEngine` render-thread tap. Hopping to an actor from that thread
/// would introduce an unbounded await inside a realtime callback. The lock-free
/// ring buffer on the C++ side is what makes this safe.
actor ScribaticEngine {

    /// ARC-managed via `SWIFT_SHARED_REFERENCE`; no manual retain/release here.
    ///
    /// `nonisolated(unsafe)` because `pushAudio` is called from the
    /// `AVAudioEngine` render thread and cannot hop to this actor — see the
    /// note on that method. What makes the access safe is the lock-free SPSC
    /// ring buffer on the C++ side, an invariant the compiler cannot see, so
    /// it is asserted here instead of checked.
    ///
    /// The assertion covers exactly the three `nonisolated` entry points below
    /// (`pushAudio`, `requestCancel`, `canIdentifySpeakers`). The third is safe
    /// because it touches no mutable engine state: it stats two paths fixed at
    /// construction. Adding a fourth means making the same argument for it —
    /// `drainSegments()` is only lock-guarded, and the lifecycle calls
    /// synchronise nothing at all.
    ///
    /// TODO(backend): once `EngineImpl` synchronises every entry point, this
    /// becomes `nonisolated let` with `SWIFT_SENDABLE` on the C++ class, and
    /// the compiler checks the claim instead of taking it on trust.
    nonisolated(unsafe) private let engine: EngineInterface

    private(set) var state: EngineState = .Idle

    // MARK: - Lifecycle

    init(configuration: Configuration) throws {
        var status = EngineStatus.Ok
        guard let engine = EngineInterface.create(configuration.asCxxConfig(), &status) else {
            throw ScribaticEngineError(status: status)
        }
        self.engine = engine
    }

    /// Maps the GGUF weights and primes the KV cache.
    func warmUp() throws {
        let status = engine.warmUp()
        guard status == .Ok else { throw ScribaticEngineError(status: status) }
        state = .Listening
    }

    /// Called on `scenePhase == .background` so the resident set shrinks before
    /// the jetsam daemon takes an interest in the process.
    func hibernate() {
        engine.hibernate()
        state = .Idle
    }

    // MARK: - Sessions

    /// Before the tap starts: resets the ring buffer and the timeline, so
    /// segment times line up with the file about to be written.
    func beginSession() {
        engine.beginSession()
    }

    /// "auto" or an ISO 639-1 code; applies from the next `beginSession()`.
    func setLanguage(_ code: String) {
        engine.setLanguage(std.string(code))
    }

    /// The recording's language, or empty while it is still being detected.
    func sessionLanguage() -> String {
        String(engine.sessionLanguage())
    }

    /// Persists the finished session. Call after `flush()`.
    func saveSession(title: String, createdAt: Date, audioURL: URL?) throws -> Int64 {
        let id = engine.saveSession(
            std.string(title),
            Int64(createdAt.timeIntervalSince1970),
            std.string(audioURL?.path(percentEncoded: false) ?? "")
        )
        guard id > 0 else { throw ScribaticEngineError(status: .DatabaseFailed) }
        return id
    }

    // MARK: - Realtime ingress

    /// Realtime-safe. Invoked directly from the audio tap thread.
    nonisolated func pushAudio(_ buffer: UnsafePointer<Float>, frameCount: Int) {
        _ = engine.pushAudio(buffer, frameCount)
    }

    // MARK: - Inference

    /// Decodes whatever audio is still buffered, however short, and returns
    /// what it produced. Called when recording stops: without it the audio
    /// accumulated since the last full decode window is discarded, which reads
    /// as the app losing the end of the last sentence.
    func flush() throws -> [TranscriptSegmentValue] {
        let status = engine.flush()
        guard status == .Ok || status == .Cancelled else {
            throw ScribaticEngineError(status: status)
        }
        return engine.drainSegments().map(TranscriptSegmentValue.init)
    }

    /// One encode/decode pass, returning finalised segments.
    ///
    /// Returning a value instead of invoking a callback is deliberate: it keeps
    /// the C++ header free of `std::function`, which the Swift importer cannot
    /// model, and it removes any question of which executor a callback lands on.
    func transcribe() throws -> [TranscriptSegmentValue] {
        try Task.checkCancellation()
        let status = engine.runTranscriptionPass()
        guard status == .Ok || status == .Cancelled else {
            throw ScribaticEngineError(status: status)
        }
        return engine.drainSegments().map(TranscriptSegmentValue.init)
    }

    /// Continuous stream for the UI to consume with `for await`.
    /// Back-pressure is `.bufferingNewest(1)`: if the view is busy rendering,
    /// stale partial hypotheses are dropped rather than queued.
    func transcriptionStream() -> AsyncThrowingStream<[TranscriptSegmentValue], Error> {
        AsyncThrowingStream(bufferingPolicy: .bufferingNewest(1)) { continuation in
            let task = Task.detached(priority: .userInitiated) { [weak self] in
                guard let self else { return }
                while !Task.isCancelled {
                    do {
                        let batch = try await self.transcribe()
                        if !batch.isEmpty { continuation.yield(batch) }
                        try await Task.sleep(for: .milliseconds(400))
                    } catch {
                        continuation.finish(throwing: error)
                        return
                    }
                }
                continuation.finish()
            }
            continuation.onTermination = { _ in task.cancel() }
        }
    }

    // MARK: - Notes

    func listNotes() -> [NoteSummaryValue] {
        engine.listNotes().map(NoteSummaryValue.init)
    }

    func loadNote(_ id: Int64) -> NoteDetailValue? {
        NoteDetailValue(engine.loadNote(id))
    }

    func deleteRecording(_ id: Int64) throws {
        try check(engine.deleteRecording(id))
    }

    func deleteNote(_ id: Int64) throws {
        try check(engine.deleteNote(id))
    }

    func exportTranscript(_ id: Int64, includeTimestamps: Bool, anonymise: Bool) -> String {
        String(engine.exportTranscript(id, includeTimestamps, anonymise))
    }

    // MARK: - Speakers

    /// `nonisolated` because it only stats two files — no engine state.
    nonisolated var canIdentifySpeakers: Bool {
        engine.canIdentifySpeakers()
    }

    /// Diarizes the note's recording. Seconds of work per minute of audio, off
    /// this actor: a new recording's transcription passes must not wait for
    /// it. The C++ side uses a model of its own for this. `expected` 0
    /// estimates the count.
    nonisolated func identifySpeakers(_ id: Int64, expected: Int32 = 0) async throws {
        nonisolated(unsafe) let engine = self.engine
        let status = await Task.detached(priority: .utility) { engine.identifySpeakers(id, expected) }.value
        guard status == .Ok else { throw ScribaticEngineError(status: status) }
    }

    // MARK: - Final transcript

    /// Whether the accurate model is on the device; stats a file.
    nonisolated var canRefine: Bool { engine.canRefine() }

    /// The whole recording again with the accurate model, replacing the live
    /// preview. Minutes for a long recording, so off this actor, like
    /// `identifySpeakers`; the engine loads its own model for it.
    nonisolated func refineTranscript(_ id: Int64) async throws {
        nonisolated(unsafe) let engine = self.engine
        let status = await Task.detached(priority: .utility) { engine.refineTranscript(id) }.value
        guard status == .Ok else { throw ScribaticEngineError(status: status) }
    }

    nonisolated var refineProgress: Float { engine.refineProgress() }

    nonisolated func cancelRefine() { engine.cancelRefine() }

    // MARK: - Correcting speakers

    /// `speaker` < 0 makes a new speaker for the segment.
    func setSegmentSpeaker(_ id: Int64, segment: Int64, speaker: Int32) throws {
        try check(engine.setSegmentSpeaker(id, segment, speaker))
    }

    func mergeSpeakers(_ id: Int64, speakers: [Int32], into: Int32) throws {
        var list = scribatic.core.SpeakerList()
        for speaker in speakers { list.push_back(speaker) }
        try check(engine.mergeSpeakers(id, list, into))
    }

    func setNoteLayout(_ id: Int64, layout: String) throws {
        try check(engine.setNoteLayout(id, std.string(layout)))
    }

    func renameSpeaker(_ id: Int64, speaker: Int32, name: String) throws {
        try check(engine.renameSpeaker(id, speaker, std.string(name)))
    }

    private func check(_ status: EngineStatus) throws {
        guard status == .Ok else { throw ScribaticEngineError(status: status) }
    }

    func search(query: String, topK: Int32 = 8) -> [RetrievalHitValue] {
        engine.search(std.string(query), topK).map(RetrievalHitValue.init)
    }

    /// Cooperative cancellation, observed by the ggml abort callback.
    nonisolated func requestCancel() {
        engine.requestCancel()
    }
}
