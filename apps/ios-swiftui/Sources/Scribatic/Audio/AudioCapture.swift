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
///
/// ## When iOS takes the microphone away
/// A call, Siri or an alarm interrupts the session, and the engine stops
/// without a word; so does a change of input hardware, such as AirPods
/// connecting. Interruptions are reported through `onEvent` for the recorder
/// to pause and resume; a hardware change is handled here, by rebuilding the
/// tap for the new input and carrying on into the same file.
final class AudioCapture {

    enum Event {
        /// Another app or a call has the microphone; capture has stopped.
        case interruptionBegan
        /// The microphone is back. `shouldResume` is iOS saying recording
        /// may pick up again without the user asking.
        case interruptionEnded(shouldResume: Bool)
        /// Capture cannot continue; what was recorded is intact.
        case failed(String)
    }

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

    /// New for every recording: one that lived through a media services
    /// reset is unusable, and there is no telling which those are.
    private var audioEngine = AVAudioEngine()
    private let writeQueue = DispatchQueue(label: "com.scribatic.audio.write", qos: .utility)

    private var converter: AVAudioConverter?
    private var file: AVAudioFile?
    private var sink: (@Sendable (UnsafePointer<Float>, Int) -> Void)?
    private var observers: [NSObjectProtocol] = []

    private(set) var isRunning = false
    private var isPaused = false
    /// Set on the write queue once a write has failed.
    private var writeFailed = false
    private(set) var recordingURL: URL?

    /// Delivered on the main actor, from the time capture starts until it stops.
    var onEvent: (@MainActor (Event) -> Void)?

    #if DEBUG
    private var injected: InjectedAudioSource?
    #endif

    /// How the capture is stored: WAV, mono 16 kHz float32 — the engine's own
    /// input format. The core maps the file straight into speaker diarization
    /// without converting or copying it (ADR-009), and the same file plays
    /// back through AVAudioPlayer. The core parses the chunk list, so the
    /// padding AVAudioFile inserts before `data` is not a problem.
    private static var fileSettings: [String: Any] { [
        AVFormatIDKey: kAudioFormatLinearPCM,
        AVSampleRateKey: 16_000,
        AVNumberOfChannelsKey: 1,
        AVLinearPCMBitDepthKey: 32,
        AVLinearPCMIsFloatKey: true,
        AVLinearPCMIsBigEndianKey: false,
        AVLinearPCMIsNonInterleaved: false,
    ] }

    /// A new, unique file in `directory`, the engine's recordings directory.
    /// The `recording-` prefix and `.wav` suffix are part of the contract: the
    /// core only ever sweeps files named that way.
    static func newRecordingURL(in directory: URL) -> URL {
        let stamp = Int(Date().timeIntervalSince1970 * 1000)
        return directory.appending(path: "recording-\(stamp).wav")
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

    /// Starts the tap, writing the capture to `url`. `sink` is invoked on the
    /// render thread — it must do no more than hand the frames to the engine.
    func start(writingTo url: URL, sink: @escaping @Sendable (UnsafePointer<Float>, Int) -> Void) throws {
        guard !isRunning else { return }

        #if DEBUG
        if let source = InjectedAudioSource.configured() {
            self.file = try AVAudioFile(forWriting: url, settings: Self.fileSettings)
            self.recordingURL = url
            injected = source
            // Same sharing as the tap closure below, which the compiler does
            // not check because installTap's block is not @Sendable: the
            // source thread only converts, sinks, and queues the file write.
            nonisolated(unsafe) let capture = self
            try source.start { buffer in
                capture.deliver(buffer, sink: sink)
            }
            isRunning = true
            return
        }
        #endif

        let session = AVAudioSession.sharedInstance()
        // .playAndRecord so playback of a finished recording does not require
        // tearing the session down and rebuilding it.
        try session.setCategory(.playAndRecord, mode: .default, options: [.defaultToSpeaker, .allowBluetooth])
        try session.setActive(true)

        audioEngine = AVAudioEngine()
        writeFailed = false
        self.sink = sink
        try installTap()

        self.file = try AVAudioFile(forWriting: url, settings: Self.fileSettings)
        self.recordingURL = url

        audioEngine.prepare()
        try audioEngine.start()
        isRunning = true
        isPaused = false
        observe()
    }

    /// Leaves the tap installed and the file open; only the engine is paused, so
    /// resuming does not start a new recording.
    func pause() {
        guard isRunning else { return }
        isPaused = true
        #if DEBUG
        if let injected { injected.pause(); return }
        #endif
        audioEngine.pause()
    }

    func resume() throws {
        guard isRunning else { return }
        #if DEBUG
        if let injected { injected.resume(); isPaused = false; return }
        #endif
        // An interruption deactivated the session; after a manual pause this
        // is a no-op.
        try AVAudioSession.sharedInstance().setActive(true)
        try audioEngine.start()
        isPaused = false
    }

    func stop() {
        guard isRunning else { return }
        isRunning = false
        isPaused = false

        #if DEBUG
        if let injected {
            injected.stop()
            self.injected = nil
            writeQueue.sync { self.file = nil }
            return
        }
        #endif

        observers.forEach(NotificationCenter.default.removeObserver)
        observers = []
        audioEngine.inputNode.removeTap(onBus: 0)
        audioEngine.stop()

        // Close the file on the same queue the writes go through, so it is not
        // released while a write is still in flight.
        writeQueue.sync { self.file = nil }
        converter = nil
        sink = nil

        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
    }

    // MARK: - Interruptions and hardware changes

    /// The converter is built for the input's current format, which is the
    /// hardware's: it changes when the input does.
    private func installTap() throws {
        guard let sink else { return }
        let input = audioEngine.inputNode
        let inputFormat = input.outputFormat(forBus: 0)
        guard inputFormat.sampleRate > 0, inputFormat.channelCount > 0 else {
            throw CaptureError.noInputAvailable
        }
        guard let converter = AVAudioConverter(from: inputFormat, to: Self.targetFormat) else {
            throw CaptureError.converterUnavailable
        }
        self.converter = converter

        input.installTap(onBus: 0, bufferSize: 4096, format: inputFormat) { [weak self] buffer, _ in
            guard let self else { return }
            self.handle(buffer: buffer, sink: sink)
        }
    }

    private func observe() {
        let center = NotificationCenter.default
        // The blocks are @Sendable and this class is not; they run on the
        // main queue, which is where every other use of it happens.
        nonisolated(unsafe) let capture = self

        observers.append(center.addObserver(
            forName: AVAudioSession.interruptionNotification, object: nil, queue: .main
        ) { note in
            let info = note.userInfo
            guard let raw = info?[AVAudioSessionInterruptionTypeKey] as? UInt,
                  let type = AVAudioSession.InterruptionType(rawValue: raw) else { return }
            let options = AVAudioSession.InterruptionOptions(
                rawValue: info?[AVAudioSessionInterruptionOptionKey] as? UInt ?? 0)
            MainActor.assumeIsolated {
                switch type {
                case .began:
                    capture.onEvent?(.interruptionBegan)
                case .ended:
                    capture.onEvent?(.interruptionEnded(shouldResume: options.contains(.shouldResume)))
                @unknown default:
                    break
                }
            }
        })

        observers.append(center.addObserver(
            forName: .AVAudioEngineConfigurationChange, object: audioEngine, queue: .main
        ) { _ in
            MainActor.assumeIsolated { capture.inputChanged() }
        })

        observers.append(center.addObserver(
            forName: AVAudioSession.mediaServicesWereResetNotification, object: nil, queue: .main
        ) { _ in
            MainActor.assumeIsolated {
                capture.onEvent?(.failed("The microphone stopped working."))
            }
        })
    }

    /// The engine has stopped itself for new input hardware. A new tap for the
    /// new format, then on into the same file — unless paused, when resuming
    /// starts the engine.
    @MainActor private func inputChanged() {
        guard isRunning else { return }
        audioEngine.inputNode.removeTap(onBus: 0)
        do {
            try installTap()
            if !isPaused {
                audioEngine.prepare()
                try audioEngine.start()
            }
        } catch {
            onEvent?(.failed("The microphone stopped working."))
        }
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

        guard error == nil, converted.frameLength > 0 else { return }
        deliver(converted, sink: sink)
    }

    /// Hands 16 kHz mono frames to the engine, then queues them for the file.
    /// Render thread (or the injected source's thread in debug builds).
    private func deliver(_ converted: AVAudioPCMBuffer, sink: (UnsafePointer<Float>, Int) -> Void) {
        guard let channel = converted.floatChannelData?[0] else { return }

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
            guard let self, let file = self.file else { return }
            do {
                try file.write(from: copy)
            } catch {
                // Storage full, most likely. Once: every later buffer fails too.
                guard !self.writeFailed else { return }
                self.writeFailed = true
                nonisolated(unsafe) let capture = self
                DispatchQueue.main.async {
                    MainActor.assumeIsolated {
                        capture.onEvent?(.failed("The recording could not be written to storage, so it stopped. Is the device full?"))
                    }
                }
            }
        }
    }
}
