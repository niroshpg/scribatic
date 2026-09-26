#if DEBUG
import AVFoundation
import Foundation

/// Debug-only stand-in for the microphone: plays a 16 kHz mono file into the
/// capture pipeline at real-time pace, as if someone were speaking it.
///
/// Simulators have no voices to record, and a speaker-identification feature
/// cannot be exercised with one person talking at a laptop. With this, a real
/// multi-speaker conversation goes through exactly the path a recording does —
/// engine ring buffer, recording file, flush, save, diarization — and only
/// the source of the samples differs. Compiled out of release builds entirely.
///
/// Enabled with a launch argument naming a file inside the app container:
///
///     xcrun simctl launch booted com.scribatic.app \
///         -ScribaticInjectAudio "Library/Caches/conversation.wav"
final class InjectedAudioSource: @unchecked Sendable {

    /// Relative to the app's home directory, or absolute.
    static func configured() -> InjectedAudioSource? {
        guard let path = UserDefaults.standard.string(forKey: "ScribaticInjectAudio") else { return nil }
        let url = path.hasPrefix("/") ? URL(filePath: path)
                                      : URL(filePath: NSHomeDirectory()).appending(path: path)
        guard FileManager.default.fileExists(atPath: url.path(percentEncoded: false)) else { return nil }
        return InjectedAudioSource(url: url)
    }

    private let url: URL
    private let lock = NSLock()
    private var running = false
    private var paused = false
    private var thread: Thread?

    private init(url: URL) {
        self.url = url
    }

    /// `deliver` is called every 100 ms with a 16 kHz mono float buffer.
    func start(deliver: @escaping @Sendable (AVAudioPCMBuffer) -> Void) throws {
        let file = try AVAudioFile(forReading: url)
        let format = file.processingFormat
        guard format.sampleRate == 16_000, format.channelCount == 1 else {
            throw CocoaError(.fileReadCorruptFile)
        }

        lock.withLock { running = true; paused = false }
        let thread = Thread { [weak self] in
            let frames: AVAudioFrameCount = 1_600
            var next = Date()
            while let self, self.lock.withLock({ self.running }) {
                next += 0.1
                Thread.sleep(until: next)
                if self.lock.withLock({ self.paused }) { continue }
                guard let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: frames),
                      (try? file.read(into: buffer, frameCount: frames)) != nil,
                      buffer.frameLength > 0 else {
                    continue      // file finished: silence until the user stops
                }
                deliver(buffer)
            }
        }
        thread.name = "scribatic-injected-audio"
        thread.qualityOfService = .userInitiated
        self.thread = thread
        thread.start()
    }

    func pause() { lock.withLock { paused = true } }
    func resume() { lock.withLock { paused = false } }
    func stop() { lock.withLock { running = false } }
}
#endif
