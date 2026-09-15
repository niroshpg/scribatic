import AVFoundation
import Foundation

/// Microphone capture: session, permission, tap, and the 16 kHz mono conversion
/// the engine expects.
///
/// ## The render thread
/// `installTap` calls back on AVAudioEngine's render thread. Two things happen
/// there and nothing else: the buffer is converted, and the converted frames are
/// handed to `ScribaticEngine.pushAudio`, which is `nonisolated` and writes into
/// the lock-free ring buffer. No allocation of a new converter, no awaiting, no
/// file I/O — a blocking call on that thread is a dropout.
///
/// Persisting the audio is deliberately NOT done on the render thread. The
/// buffer is copied and the write dispatched to a serial queue, because
/// `AVAudioFile.write` touches the filesystem and has no realtime guarantee.
final class AudioCapture {

    enum CaptureError: LocalizedError {
        case permissionDenied
        case noInputAvailable
        case converterUnavailable

        var errorDescription: String? {
            switch self {
            case .permissionDenied:
                return "Microphone access is off. Enable it in Settings to record."
            case .noInputAvailable:
                return "No microphone is available on this device."
            case .converterUnavailable:
                return "The microphone format could not be converted for transcription."
            }
        }
    }

    /// What the engine core expects: 16 kHz, mono, float32.
    private static let targetFormat = AVAudioFormat(
        commonFormat: .pcmFormatFloat32,
        sampleRate: 16_000,
        channels: 1,
        interleaved: false
    )!

    private let audioEngine = AVAudioEngine()
    private let writeQueue = DispatchQueue(label: "com.scribatic.audio.write", qos: .utility)

    private var converter: AVAudioConverter?
    private var file: AVAudioFile?

    private(set) var isRunning = false
    private(set) var recordingURL: URL?

    /// Where the capture is written. Inside the app container alongside the
    /// models, never shared storage — the same guarantee the engine paths carry.
    static func newRecordingURL() throws -> URL {
        let support = try FileManager.default.url(
            for: .applicationSupportDirectory,
            in: .userDomainMask,
            appropriateFor: nil,
            create: true
        )
        let name = "recording-\(Int(Date().timeIntervalSince1970)).caf"
        return support.appending(path: name)
    }

    /// Static because it touches no instance state. As a method it would send
    /// this non-Sendable object across an await from the main actor, which
    /// Swift 6 rejects as a data race risk.
    static func requestPermission() async throws {
        switch AVAudioApplication.shared.recordPermission {
        case .granted:
            return
        case .denied:
            throw CaptureError.permissionDenied
        case .undetermined:
            let granted = await AVAudioApplication.requestRecordPermission()
            if !granted { throw CaptureError.permissionDenied }
        @unknown default:
            let granted = await AVAudioApplication.requestRecordPermission()
            if !granted { throw CaptureError.permissionDenied }
        }
    }

    /// Starts the tap. `sink` is invoked on the render thread — it must do no
    /// more than hand the frames to the engine.
    func start(sink: @escaping @Sendable (UnsafePointer<Float>, Int) -> Void) throws {
        guard !isRunning else { return }

        let session = AVAudioSession.sharedInstance()
        // .playAndRecord so playback of a finished recording does not require
        // tearing the session down and rebuilding it.
        try session.setCategory(.playAndRecord, mode: .default, options: [.defaultToSpeaker, .allowBluetooth])
        try session.setActive(true)

        let input = audioEngine.inputNode
        let inputFormat = input.outputFormat(forBus: 0)
        guard inputFormat.sampleRate > 0, inputFormat.channelCount > 0 else {
            throw CaptureError.noInputAvailable
        }

        guard let converter = AVAudioConverter(from: inputFormat, to: Self.targetFormat) else {
            throw CaptureError.converterUnavailable
        }
        self.converter = converter

        let url = try Self.newRecordingURL()
        self.file = try AVAudioFile(forWriting: url, settings: Self.targetFormat.settings)
        self.recordingURL = url

        input.installTap(onBus: 0, bufferSize: 4096, format: inputFormat) { [weak self] buffer, _ in
            guard let self else { return }
            self.handle(buffer: buffer, sink: sink)
        }

        audioEngine.prepare()
        try audioEngine.start()
        isRunning = true
    }

    /// Leaves the tap installed and the file open; only the engine is paused, so
    /// resuming does not start a new recording.
    func pause() {
        guard isRunning else { return }
        audioEngine.pause()
    }

    func resume() throws {
        guard isRunning else { return }
        try audioEngine.start()
    }

    func stop() {
        guard isRunning else { return }
        audioEngine.inputNode.removeTap(onBus: 0)
        audioEngine.stop()
        isRunning = false

        // Close the file on the same queue the writes go through, so it is not
        // released while a write is still in flight.
        writeQueue.sync { self.file = nil }
        converter = nil

        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
    }

    // MARK: - Render thread

    private func handle(buffer: AVAudioPCMBuffer, sink: (UnsafePointer<Float>, Int) -> Void) {
        guard let converter else { return }

        let ratio = Self.targetFormat.sampleRate / buffer.format.sampleRate
        let capacity = AVAudioFrameCount(Double(buffer.frameLength) * ratio) + 1024
        guard let converted = AVAudioPCMBuffer(pcmFormat: Self.targetFormat, frameCapacity: capacity) else {
            return
        }

        var consumed = false
        var error: NSError?
        converter.convert(to: converted, error: &error) { _, status in
            if consumed {
                status.pointee = .noDataNow
                return nil
            }
            consumed = true
            status.pointee = .haveData
            return buffer
        }

        guard error == nil, converted.frameLength > 0,
              let channel = converted.floatChannelData?[0] else { return }

        sink(channel, Int(converted.frameLength))

        // Copy before leaving the render thread: `converted` is reused.
        guard let copy = AVAudioPCMBuffer(pcmFormat: Self.targetFormat, frameCapacity: converted.frameLength) else {
            return
        }
        copy.frameLength = converted.frameLength
        if let dst = copy.floatChannelData?[0] {
            dst.update(from: channel, count: Int(converted.frameLength))
        }

        writeQueue.async { [weak self] in
            try? self?.file?.write(from: copy)
        }
    }
}
