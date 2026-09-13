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
    /// The assertion covers exactly the two `nonisolated` entry points below
    /// (`pushAudio`, `requestCancel`). Adding a third means arguing that the
    /// method it calls is realtime-safe too — `drainSegments()` is only
    /// lock-guarded, and the lifecycle calls synchronise nothing at all.
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

    // MARK: - Realtime ingress

    /// Realtime-safe. Invoked directly from the audio tap thread.
    nonisolated func pushAudio(_ buffer: UnsafePointer<Float>, frameCount: Int) {
        _ = engine.pushAudio(buffer, frameCount)
    }

    // MARK: - Inference

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

    func summarize(transcript: String) async throws -> String {
        try Task.checkCancellation()
        return String(engine.summarize(std.string(transcript)))
    }

    func search(query: String, topK: Int32 = 8) -> [RetrievalHitValue] {
        engine.search(std.string(query), topK).map(RetrievalHitValue.init)
    }

    /// Cooperative cancellation, observed by the ggml abort callback.
    nonisolated func requestCancel() {
        engine.requestCancel()
    }
}
