import Foundation
import ScribaticCore

// The C++ side declares everything inside `namespace scribatic::core`, which
// the importer surfaces as the nested enum `scribatic.core`. These aliases let
// call sites spell the types the way the C++ headers do, without repeating the
// namespace at every use.
typealias EngineStatus = scribatic.core.EngineStatus
typealias EngineState = scribatic.core.EngineState
typealias EngineInterface = scribatic.core.EngineInterface

/// Swift-native mirrors of the C++ value types.
///
/// The C++ structs are imported directly and are perfectly usable, but they are
/// not `Sendable` and not `Equatable`, which SwiftUI's diffing wants. Copying
/// across this boundary once per segment is cheap and keeps every type that
/// reaches a `View` a plain Swift value.
struct TranscriptSegmentValue: Identifiable, Sendable, Equatable {
    let id = UUID()
    let startMs: Int64
    let endMs: Int64
    let text: String
    let confidence: Float
    let isFinal: Bool
    /// Zero-based speaker within the note; -1 until speakers are identified.
    let speaker: Int32

    init(_ cxx: scribatic.core.TranscriptSegment) {
        self.startMs = cxx.startMs
        self.endMs = cxx.endMs
        self.text = String(cxx.text)
        self.confidence = cxx.confidence
        self.isFinal = cxx.isFinal
        self.speaker = cxx.speaker
    }
}

struct SpeakerLabelValue: Identifiable, Sendable, Equatable {
    var id: Int32 { index }
    let index: Int32
    /// What the user called them; empty when unnamed.
    let name: String
    /// `name`, or "Speaker N". Spelled once, in the C++ core.
    let displayName: String

    init(_ cxx: scribatic.core.SpeakerLabel) {
        self.index = cxx.index
        self.name = String(cxx.name)
        self.displayName = String(cxx.displayName)
    }
}

struct NoteSummaryValue: Identifiable, Sendable, Equatable {
    let id: Int64
    let title: String
    let createdAt: Date
    let durationMs: Int64
    let hasAudio: Bool
    let speakerCount: Int32
    let preview: String

    init(_ cxx: scribatic.core.NoteSummary) {
        self.id = cxx.id
        self.title = String(cxx.title)
        self.createdAt = Date(timeIntervalSince1970: TimeInterval(cxx.createdAt))
        self.durationMs = cxx.durationMs
        self.hasAudio = cxx.hasAudio
        self.speakerCount = cxx.speakerCount
        self.preview = String(cxx.preview)
    }
}

struct NoteDetailValue: Identifiable, Sendable, Equatable {
    let id: Int64
    let title: String
    let createdAt: Date
    let durationMs: Int64
    /// Absolute path of the recording; nil once it has been deleted.
    let audioURL: URL?
    let speakerCount: Int32
    let speakers: [SpeakerLabelValue]
    let segments: [TranscriptSegmentValue]
    /// Empty unless an add-on has written one.
    let summary: String
    /// ISO 639-1 code of the language it was transcribed in, e.g. "es".
    let language: String

    /// nil when the core reports no such note.
    init?(_ cxx: scribatic.core.NoteDetail) {
        guard cxx.id != 0 else { return nil }
        self.id = cxx.id
        self.title = String(cxx.title)
        self.createdAt = Date(timeIntervalSince1970: TimeInterval(cxx.createdAt))
        self.durationMs = cxx.durationMs
        let path = String(cxx.audioPath)
        self.audioURL = path.isEmpty ? nil : URL(filePath: path)
        self.speakerCount = cxx.speakerCount
        self.speakers = cxx.speakers.map(SpeakerLabelValue.init)
        self.segments = cxx.segments.map(TranscriptSegmentValue.init)
        self.summary = String(cxx.summary)
        let language = String(cxx.language)
        self.language = language.isEmpty ? "en" : language
    }

    func speakerName(_ index: Int32) -> String? {
        speakers.first { $0.index == index }?.displayName
    }
}

struct RetrievalHitValue: Identifiable, Sendable {
    var id: Int64 { chunkId }
    let noteId: Int64
    let chunkId: Int64
    let snippet: String
    let distance: Float

    init(_ cxx: scribatic.core.RetrievalHit) {
        self.noteId = cxx.noteId
        self.chunkId = cxx.chunkId
        self.snippet = String(cxx.snippet)
        self.distance = cxx.distance
    }
}

struct ScribaticEngineError: Error, CustomStringConvertible {
    let status: EngineStatus
    var description: String { String(scribatic.core.describeStatus(status)) }
}

extension ScribaticEngine {
    /// Model and database locations, all inside the app's private container
    /// with `.completeFileProtection`. Nothing is written to a shared group.
    struct Configuration: Sendable {
        var whisperModelPath: String
        var llamaModelPath: String
        var embedModelPath: String
        var databasePath: String
        var recordingsDirectory: String = ""
        var segmentationModelPath: String = ""
        var speakerEmbeddingModelPath: String = ""
        var threadCount: Int32 = 4
        var useMemoryMapping = true

        /// Like `default()`, but with each model at wherever the installer
        /// found it — its Apple-hosted pack, or an imported file.
        static func resolved(_ installer: ModelInstaller) throws -> Configuration {
            var configuration = try `default`()
            func path(_ name: String, _ fallback: String) -> String {
                guard let model = installer.catalog.first(where: { $0.fileName == name }),
                      let url = installer.fileURL(for: model) else { return fallback }
                return url.path(percentEncoded: false)
            }
            configuration.whisperModelPath = path("ggml-base.bin", configuration.whisperModelPath)
            configuration.llamaModelPath = path("insight-q4_k_m.gguf", configuration.llamaModelPath)
            configuration.embedModelPath = path("embed-minilm-l6-v2.gguf", configuration.embedModelPath)
            configuration.segmentationModelPath = path("speaker-segmentation.onnx", configuration.segmentationModelPath)
            configuration.speakerEmbeddingModelPath = path("speaker-embedding.onnx", configuration.speakerEmbeddingModelPath)
            return configuration
        }

        static func `default`() throws -> Configuration {
            let support = try FileManager.default.url(
                for: .applicationSupportDirectory,
                in: .userDomainMask,
                appropriateFor: nil,
                create: true
            )
            // percentEncoded: false is load-bearing. URL.path() defaults to
            // percentEncoded: true, which turns "Application Support" into
            // "Application%20Support" — a directory that does not exist. The
            // C++ side stats the path verbatim, so the engine reported
            // ModelNotFound no matter where the weights actually were.
            let store = try Self.privateDirectory(support.appending(path: "store"))
            let recordings = try Self.privateDirectory(support.appending(path: "recordings"))
            let models = ["ggml-base.bin", "insight-q4_k_m.gguf", "embed-minilm-l6-v2.gguf",
                          "speaker-segmentation.onnx", "speaker-embedding.onnx"]
                .map { support.appending(path: $0) }
            // Weights are re-downloadable and over a gigabyte; backing them
            // up is exactly what Apple's storage guidelines rule out.
            for var model in models where FileManager.default.fileExists(atPath: model.path(percentEncoded: false)) {
                try? Self.excludeFromBackup(&model)
            }
            return Configuration(
                whisperModelPath: models[0].path(percentEncoded: false),
                llamaModelPath: models[1].path(percentEncoded: false),
                embedModelPath: models[2].path(percentEncoded: false),
                databasePath: store.appending(path: "scribatic.sqlite").path(percentEncoded: false),
                recordingsDirectory: recordings.path(percentEncoded: false),
                segmentationModelPath: models[3].path(percentEncoded: false),
                speakerEmbeddingModelPath: models[4].path(percentEncoded: false)
            )
        }

        /// Creates `url` if needed and excludes it from iCloud and device
        /// backup. The exclusion is on the directory, so it covers everything
        /// written inside it later — including SQLite's -wal and -shm files,
        /// which a per-file exclusion made at open time would miss.
        ///
        /// Load-bearing: without it, deleting a recording removes it from the
        /// phone but not from last night's iCloud backup.
        static func privateDirectory(_ url: URL) throws -> URL {
            try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
            var url = url
            try excludeFromBackup(&url)
            return url
        }

        private static func excludeFromBackup(_ url: inout URL) throws {
            var values = URLResourceValues()
            values.isExcludedFromBackup = true
            try url.setResourceValues(values)
        }

        func asCxxConfig() -> scribatic.core.EngineConfig {
            var config = scribatic.core.EngineConfig()
            config.whisperModelPath = std.string(whisperModelPath)
            config.llamaModelPath = std.string(llamaModelPath)
            config.embedModelPath = std.string(embedModelPath)
            config.databasePath = std.string(databasePath)
            config.recordingsDirectory = std.string(recordingsDirectory)
            config.segmentationModelPath = std.string(segmentationModelPath)
            config.speakerEmbeddingModelPath = std.string(speakerEmbeddingModelPath)
            config.threadCount = threadCount
            config.useMemoryMapping = useMemoryMapping
            return config
        }
    }
}

/// One model file from the core's catalog, identified by its SHA-256.
struct ModelSpecValue: Identifiable, Sendable, Equatable {
    var id: String { fileName }
    let fileName: String
    let title: String
    let purpose: String
    /// For an optional model: what stops working without it.
    let withoutIt: String
    let sizeBytes: Int64
    let sha256: String
    let required: Bool

    init(_ cxx: scribatic.core.ModelSpec) {
        self.fileName = String(cxx.fileName)
        self.title = String(cxx.title)
        self.purpose = String(cxx.purpose)
        self.withoutIt = String(cxx.withoutIt)
        self.sizeBytes = cxx.sizeBytes
        self.sha256 = String(cxx.sha256)
        self.required = cxx.required
    }

    static func catalog() -> [ModelSpecValue] {
        scribatic.core.modelCatalog().map(ModelSpecValue.init)
    }

    static func downloadPage() -> URL? {
        URL(string: String(scribatic.core.modelDownloadPage()))
    }
}
