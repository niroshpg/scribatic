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
        ).path(percentEncoded: false)

        for path in [configuration.whisperModelPath,
                     configuration.llamaModelPath,
                     configuration.databasePath] {
            XCTAssertTrue(path.hasPrefix(support), "\(path) escapes the app container")
        }
    }

    /// Regression guard. `URL.path()` defaults to percentEncoded: true, which
    /// renders "Application Support" as "Application%20Support". The C++ side
    /// stats the string verbatim, so an encoded path means the engine reports
    /// ModelNotFound forever, no matter where the weights are placed. The app
    /// built and launched cleanly with this bug; only running it revealed it.
    func testConfigurationPathsAreNotPercentEncoded() throws {
        let configuration = try ScribaticEngine.Configuration.default()

        for path in [configuration.whisperModelPath,
                     configuration.llamaModelPath,
                     configuration.databasePath] {
            XCTAssertFalse(path.contains("%20"), "\(path) is percent-encoded; stat() will not find it")
            XCTAssertFalse(path.contains("%"), "\(path) looks percent-encoded")
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
