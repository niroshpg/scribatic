import AVFoundation
import Foundation

/// Audio recorded elsewhere (a voice memo, a call, a lecture), converted to
/// what the app records: 16 kHz mono float32 WAV, in the recordings
/// directory, so the transcript, speakers, playback and sharing all treat it
/// exactly like a recording made here.
///
/// AVFoundation reads the source, whatever it is (M4A, MP3, WAV, AIFF, CAF),
/// and resamples it. Nothing leaves the device.
enum AudioImport {

    enum ImportError: LocalizedError {
        case unreadable
        case notEnoughSpace(needed: Int64)

        var errorDescription: String? {
            switch self {
            case .unreadable:
                return "This file's audio can't be read. Try an M4A, MP3 or WAV file."
            case let .notEnoughSpace(needed):
                return "There isn't enough free storage: it needs about \(ByteCountFormatter.string(fromByteCount: needed, countStyle: .file))."
            }
        }
    }

    private static let target = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: 16_000,
                                              channels: 1, interleaved: false)!

    /// Length in seconds, for the storage check before converting.
    static func duration(of source: URL) throws -> Double {
        let file = try AVAudioFile(forReading: source)
        return Double(file.length) / file.processingFormat.sampleRate
    }

    /// Writes `source` to `destination` as a recording; `progress` gets 0...1.
    static func convert(_ source: URL, to destination: URL, progress: (Double) -> Void) throws {
        let input: AVAudioFile
        do { input = try AVAudioFile(forReading: source) } catch { throw ImportError.unreadable }
        let needed = Int64(Double(input.length) / input.processingFormat.sampleRate * Double(DeviceStatus.bytesPerSecond))
        guard DeviceStatus.freeBytes() >= needed + DeviceStatus.minimumFree else {
            throw ImportError.notEnoughSpace(needed: needed)
        }
        guard let converter = AVAudioConverter(from: input.processingFormat, to: target) else {
            throw ImportError.unreadable
        }
        let settings: [String: Any] = [
            AVFormatIDKey: kAudioFormatLinearPCM,
            AVSampleRateKey: 16_000,
            AVNumberOfChannelsKey: 1,
            AVLinearPCMBitDepthKey: 32,
            AVLinearPCMIsFloatKey: true,
            AVLinearPCMIsBigEndianKey: false,
            AVLinearPCMIsNonInterleaved: false,
        ]
        do {
            let output = try AVAudioFile(forWriting: destination, settings: settings)
            let chunk: AVAudioFrameCount = 32_768
            guard let source = AVAudioPCMBuffer(pcmFormat: input.processingFormat, frameCapacity: chunk),
                  let converted = AVAudioPCMBuffer(pcmFormat: target, frameCapacity: chunk) else {
                throw ImportError.unreadable
            }
            var finished = false
            var readError: Error?
            while !finished {
                var status = AVAudioConverterOutputStatus.haveData
                var error: NSError?
                status = converter.convert(to: converted, error: &error) { _, outStatus in
                    do {
                        try input.read(into: source, frameCount: chunk)
                    } catch {
                        readError = error
                        outStatus.pointee = .endOfStream
                        return nil
                    }
                    if source.frameLength == 0 {
                        outStatus.pointee = .endOfStream
                        return nil
                    }
                    outStatus.pointee = .haveData
                    return source
                }
                if let error { throw error }
                if let readError { throw readError }
                if converted.frameLength > 0 { try output.write(from: converted) }
                progress(input.length > 0 ? Double(input.framePosition) / Double(input.length) : 1)
                finished = status == .endOfStream || status == .error
            }
        } catch let error as ImportError {
            try? FileManager.default.removeItem(at: destination)
            throw error
        } catch {
            try? FileManager.default.removeItem(at: destination)
            throw error
        }
    }
}
