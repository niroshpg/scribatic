import Foundation
import os

/// Free storage, what Scribatic itself keeps, and memory: for the Storage &
/// memory panel, and the checks before and during a recording.
///
/// A recording is stored as 16 kHz mono float32, what the speaker step reads
/// in place, so it takes 64 kB a second: 230 MB an hour. That is the number
/// every estimate here is built on.
struct DeviceStatus: Sendable {
    static let bytesPerSecond: Int64 = 64_000
    /// Below this a recording does not start, and one in progress is stopped
    /// and saved before writes begin to fail.
    static let minimumFree: Int64 = 50_000_000
    /// Choices for the warning before recording; 1 GB is about 4 hours.
    static let warningLevels: [Int64] = [500_000_000, 1_000_000_000, 2_000_000_000]

    static var warningLevel: Int64 {
        get {
            let stored = Int64(UserDefaults.standard.integer(forKey: "storageWarningBytes"))
            return warningLevels.contains(stored) ? stored : 1_000_000_000
        }
        set { UserDefaults.standard.set(Int(newValue), forKey: "storageWarningBytes") }
    }

    let freeBytes: Int64
    let recordingsBytes: Int64
    let recordingCount: Int
    let modelsBytes: Int64
    let notesBytes: Int64
    /// What this process may still allocate before iOS ends it.
    let availableMemory: Int64
    let totalMemory: Int64

    /// Roughly how much can still be recorded before the minimum is reached.
    var recordingTimeLeft: TimeInterval {
        Double(max(0, freeBytes - Self.minimumFree)) / Double(Self.bytesPerSecond)
    }

    static func current(_ configuration: ScribaticEngine.Configuration?) -> DeviceStatus {
        let recordings = configuration.map { URL(filePath: $0.recordingsDirectory) }
        let files = recordings.flatMap {
            try? FileManager.default.contentsOfDirectory(at: $0, includingPropertiesForKeys: [.fileSizeKey])
        } ?? []
        let models = configuration.map {
            [$0.whisperModelPath, $0.accurateModelPath, $0.segmentationModelPath,
             $0.speakerEmbeddingModelPath, $0.embedModelPath, $0.llamaModelPath]
        } ?? []
        let store = configuration.map { URL(filePath: $0.databasePath).deletingLastPathComponent() }
        return DeviceStatus(
            freeBytes: freeBytes(),
            recordingsBytes: files.map(size).reduce(0, +),
            recordingCount: files.filter { ["wav", "m4a"].contains($0.pathExtension) }.count,
            modelsBytes: models.map { size(URL(filePath: $0)) }.reduce(0, +),
            notesBytes: store.map(directorySize) ?? 0,
            availableMemory: Int64(os_proc_available_memory()),
            totalMemory: Int64(ProcessInfo.processInfo.physicalMemory)
        )
    }

    /// Space the system would make available for something the user asked
    /// for, which counts purgeable caches: what Settings shows as free.
    static func freeBytes() -> Int64 {
        let home = URL(filePath: NSHomeDirectory())
        let values = try? home.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
        return values?.volumeAvailableCapacityForImportantUsage ?? 0
    }

    private static func size(_ url: URL) -> Int64 {
        Int64((try? url.resourceValues(forKeys: [.fileSizeKey]))?.fileSize ?? 0)
    }

    private static func directorySize(_ url: URL) -> Int64 {
        let files = (try? FileManager.default.contentsOfDirectory(at: url, includingPropertiesForKeys: [.fileSizeKey])) ?? []
        return files.map(size).reduce(0, +)
    }
}

/// "About 3 hours", "About 40 minutes".
func durationEstimate(_ seconds: TimeInterval) -> String {
    let formatter = DateComponentsFormatter()
    formatter.unitsStyle = .full
    formatter.maximumUnitCount = 1
    formatter.allowedUnits = seconds >= 3600 ? [.hour] : [.minute]
    return "About " + (formatter.string(from: max(60, seconds)) ?? "")
}
