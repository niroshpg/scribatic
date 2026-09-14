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

    init(_ cxx: scribatic.core.TranscriptSegment) {
        self.startMs = cxx.startMs
        self.endMs = cxx.endMs
        self.text = String(cxx.text)
        self.confidence = cxx.confidence
        self.isFinal = cxx.isFinal
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
        var databasePath: String
        var threadCount: Int32 = 4
        var useMemoryMapping = true

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
            return Configuration(
                whisperModelPath: support.appending(path: "ggml-base.en.bin").path(percentEncoded: false),
                llamaModelPath: support.appending(path: "insight-q4_k_m.gguf").path(percentEncoded: false),
                databasePath: support.appending(path: "scribatic.sqlite").path(percentEncoded: false)
            )
        }

        func asCxxConfig() -> scribatic.core.EngineConfig {
            var config = scribatic.core.EngineConfig()
            config.whisperModelPath = std.string(whisperModelPath)
            config.llamaModelPath = std.string(llamaModelPath)
            config.databasePath = std.string(databasePath)
            config.threadCount = threadCount
            config.useMemoryMapping = useMemoryMapping
            return config
        }
    }
}
