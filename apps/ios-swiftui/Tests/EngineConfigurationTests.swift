import XCTest
@testable import ScribaticApp

/// Covers the Swift-side half of the engine configuration.
///
/// Note on scope: `asCxxConfig()` is deliberately NOT tested here. Its return
/// type is `scribatic.core.EngineConfig`, and a member whose signature names a
/// C++ type is not visible across a module boundary — even under `@testable`.
/// Making it visible would mean `@_exported import ScribaticCore` from the app,
/// which would put `ggml`/`llama` types back into app-facing module surface and
/// undo the containment the module map exists to enforce. Marshalling coverage
/// belongs in the dependency-free host binary under `core/engine/tests/`,
/// on the C++ side of the boundary where the types actually live.
final class EngineConfigurationTests: XCTestCase {

    /// Everything the engine writes must live in the app's private container.
    /// A path that escapes it would defeat the file-protection guarantee.
    func testDefaultConfigurationStaysInApplicationSupport() throws {
        let configuration = try ScribaticEngine.Configuration.default()
        let support = try FileManager.default.url(
            for: .applicationSupportDirectory,
            in: .userDomainMask,
            appropriateFor: nil,
            create: true
        ).path()

        for path in [configuration.whisperModelPath,
                     configuration.llamaModelPath,
                     configuration.databasePath] {
            XCTAssertTrue(path.hasPrefix(support), "\(path) escapes the app container")
        }
    }

    func testDefaultThreadCountLeavesHeadroomForAudio() {
        let configuration = ScribaticEngine.Configuration(
            whisperModelPath: "", llamaModelPath: "", databasePath: ""
        )
        XCTAssertEqual(configuration.threadCount, 4)
        XCTAssertTrue(configuration.useMemoryMapping)
    }
}
