import AVFoundation
import Foundation

/// How recordings are kept. A recording is made as WAV (16 kHz mono float32,
/// what the engine reads, and recoverable if the app dies mid-recording) and,
/// once its note is finished, kept as M4A: AAC at `aacBitRate`, about 21 MB an
/// hour instead of 230. The engine does not decode M4A, so improving a
/// compressed note's transcript or identifying its speakers again decodes it
/// to a temporary WAV first.
enum RecordingFiles {
    /// Measured on quiet and full-level conversations, levelled first:
    /// transcripts from 48 kbit/s differ from the WAV's by 0-11% of words
    /// and kept the speaker count in 2 of 3; 32 kbit/s, 2-14% and 1 of 3.
    static let aacBitRate = 48_000

    static func isCompressed(_ url: URL) -> Bool { url.pathExtension.lowercased() == "m4a" }

    /// Compresses `wav` to an M4A of the same name beside it. Written in the
    /// temporary directory and moved into place, so a half-written file never
    /// sits among the recordings. Blocking: seconds per hour of audio.
    static func compress(_ wav: URL) throws -> URL {
        let temporary = FileManager.default.temporaryDirectory.appending(path: "compress-\(UUID().uuidString).m4a")
        defer { try? FileManager.default.removeItem(at: temporary) }
        try ShareFiles.encodeM4A(wav: wav, to: temporary, gain: try levelling(wav))
        let target = wav.deletingPathExtension().appendingPathExtension("m4a")
        try? FileManager.default.removeItem(at: target)
        try FileManager.default.moveItem(at: temporary, to: target)
        return target
    }

    /// The gain that brings `wav`'s peak to -1 dBFS, at most 40 dB. AAC at
    /// this rate drops what it judges inaudible, and a quiet recording (a
    /// phone across the room from a television, peaking at -30 dBFS) lost its
    /// speech with it; raised first, it keeps it. Recognition does not depend
    /// on level, and playback is easier to hear. Never lowers a loud one.
    static func levelling(_ wav: URL) throws -> Float {
        let input = try AVAudioFile(forReading: wav)
        guard let buffer = AVAudioPCMBuffer(pcmFormat: input.processingFormat, frameCapacity: 65_536) else { return 1 }
        var peak: Float = 0
        while input.framePosition < input.length {
            try input.read(into: buffer)
            if buffer.frameLength == 0 { break }
            guard let channels = buffer.floatChannelData else { return 1 }
            for channel in 0..<Int(buffer.format.channelCount) {
                for i in 0..<Int(buffer.frameLength) { peak = max(peak, abs(channels[channel][i])) }
            }
        }
        guard peak > 0 else { return 1 }
        return min(max(0.89 / peak, 1), 100)
    }

    /// Runs `work` with the recording as the engine can read it: nil for a
    /// WAV (the engine reads the note's own file), else the M4A decoded to a
    /// temporary WAV, deleted afterwards.
    @MainActor
    static func withEngineAudio<T>(_ url: URL?, _ work: (URL?) async throws -> T) async throws -> T {
        guard let url, isCompressed(url) else { return try await work(nil) }
        let decoded = FileManager.default.temporaryDirectory.appending(path: "decoded-\(UUID().uuidString).wav")
        defer { try? FileManager.default.removeItem(at: decoded) }
        try await Task.detached(priority: .utility) { try AudioImport.convert(url, to: decoded) { _ in } }.value
        return try await work(decoded)
    }
}

/// How long finished recordings are kept; transcripts are always kept.
enum KeepRecordings: String, CaseIterable, Identifiable {
    case always
    case month
    case untilTranscribed

    var id: String { rawValue }

    var label: String {
        switch self {
        case .always: "Always"
        case .month: "30 days"
        case .untilTranscribed: "Until transcribed"
        }
    }

    var explanation: String {
        switch self {
        case .always:
            "Recordings are kept, compressed (about 21 MB an hour), until you delete them."
        case .month:
            "A recording is deleted 30 days after it was made. Its transcript, speakers and summary stay."
        case .untilTranscribed:
            "A recording is deleted once its transcript is finished. Its transcript, speakers and summary stay, but it can't be played, improved or have its speakers identified again."
        }
    }

    private static let key = "keepRecordings"

    static var current: KeepRecordings {
        get { UserDefaults.standard.string(forKey: key).flatMap(KeepRecordings.init(rawValue:)) ?? .always }
        set { UserDefaults.standard.set(newValue.rawValue, forKey: key) }
    }

    static var monthAgo: Date { Date().addingTimeInterval(-30 * 24 * 3600) }
}


/// "Identify speakers": straight after each recording, or only when asked from
/// the note's menu. Working out who spoke takes minutes on a slow device, and
/// someone recording alone never needs it.
enum SpeakerIdentification: String, CaseIterable, Identifiable {
    case automatic
    case onRequest

    var id: String { rawValue }

    var label: String {
        switch self {
        case .automatic: "Automatically"
        case .onRequest: "Only when I ask"
        }
    }

    var explanation: String {
        switch self {
        case .automatic:
            "Who said what is worked out once each transcript is finished."
        case .onRequest:
            "Speakers are identified only when you choose Identify speakers in a note's menu. With Keep recordings set to Until transcribed, the recording is gone by then, so this can't be done later."
        }
    }

    private static let key = "identifySpeakers"

    static var current: SpeakerIdentification {
        get { UserDefaults.standard.string(forKey: key).flatMap(SpeakerIdentification.init(rawValue:)) ?? .automatic }
        set { UserDefaults.standard.set(newValue.rawValue, forKey: key) }
    }
}
