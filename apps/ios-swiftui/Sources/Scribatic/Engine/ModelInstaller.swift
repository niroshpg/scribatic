import BackgroundAssets
import CryptoKit
import Foundation
import System

/// Brings model files into Application Support from files the user picked.
///
/// The app makes no network requests and downloads nothing. The user downloads
/// the files in Safari, then picks them in the Files importer; each is copied
/// in and hashed on the way. A file is accepted only if its SHA-256 matches an
/// entry in the core's catalog, and it is stored under that entry's name —
/// whatever it was called when downloaded.
struct ModelInstaller: Sendable {

    let catalog = ModelSpecValue.catalog()

    private var directory: URL {
        get throws {
            try FileManager.default.url(for: .applicationSupportDirectory, in: .userDomainMask,
                                        appropriateFor: nil, create: true)
        }
    }

    /// Present, the catalog size, and actually readable. Size, not hash:
    /// hashing 1.4 GB on every launch would cost seconds, and a file only gets
    /// here by passing the hash check in `importFile` or through the store.
    /// Readable, not just present: a size comes from metadata alone, and a
    /// model the setup screen called installed must be one the engine can open.
    func isInstalled(_ model: ModelSpecValue) -> Bool {
        fileURL(for: model) != nil
    }

    /// Where the model is, if anywhere: its Apple-hosted asset pack first
    /// (ADR-011), then a file imported into Application Support. The engine
    /// maps whichever it gets, so a pack-delivered model is never copied.
    func fileURL(for model: ModelSpecValue) -> URL? {
        if let url = packFileURL(for: model), Self.check(url, model) == nil {
            return url
        }
        guard let url = try? directory.appending(path: model.fileName),
              Self.check(url, model) == nil else { return nil }
        return url
    }

    /// Each pack keeps its files in a directory named after the pack, and the
    /// file URL is built from the directory's. iOS 26.0 has a known bug in
    /// `AssetPackManager.url(for:)` for file paths (fixed in 26.1); asking for
    /// the containing directory is Apple's documented workaround. It throws
    /// when no downloaded pack holds the directory.
    private func packFileURL(for model: ModelSpecValue) -> URL? {
        guard let directory = try? AssetPackManager.shared.url(for: FilePath(Self.packID(for: model))) else {
            return nil
        }
        return directory.appending(path: model.fileName)
    }

    /// Why `url` is not a usable copy of `model`, or nil if it is: the right
    /// size, and opens for reading.
    private static func check(_ url: URL, _ model: ModelSpecValue) -> String? {
        let path = url.path(percentEncoded: false)
        guard let size = (try? FileManager.default.attributesOfItem(atPath: path)[.size]) as? Int64 else {
            return "not found"
        }
        guard size == model.sizeBytes else { return "\(size) bytes, expected \(model.sizeBytes)" }
        let fd = open(path, O_RDONLY)
        guard fd >= 0 else { return "cannot be opened (\(String(cString: strerror(errno))))" }
        close(fd)
        return nil
    }

    /// One line per wanted model saying where it was looked for and what was
    /// wrong, for the failure screen. Without it "model load failed" gives a
    /// tester nothing to report.
    func diagnosis() -> String {
        catalog.filter(isWanted).map { model in
            var line = "\(model.fileName): "
            if let url = packFileURL(for: model) {
                line += "pack \(Self.check(url, model) ?? "ok")"
            } else {
                line += "pack not on device"
            }
            if let url = try? directory.appending(path: model.fileName),
               FileManager.default.fileExists(atPath: url.path(percentEncoded: false)) {
                line += "; imported \(Self.check(url, model) ?? "ok")"
            }
            return line
        }.joined(separator: "\n") + "\niOS \(ProcessInfo.processInfo.operatingSystemVersionString)"
    }

    // MARK: - Apple-hosted asset packs

    /// The pack a model ships in; the ids in AssetPacks/*.json.
    static func packID(for model: ModelSpecValue) -> String {
        model.required ? "models-core" : "models-answers"
    }

    /// Asks the system for a pack and waits until it is on the device. Throws
    /// when this install has no Apple-hosted packs — a build from Xcode with
    /// no mock server, for instance — which is the cue to offer importing.
    func fetchPack(_ id: String) async throws {
        let pack = try await AssetPackManager.shared.assetPack(withID: id)
        try await AssetPackManager.shared.ensureLocalAvailability(of: pack)
    }

    func removePack(_ id: String) async {
        try? await AssetPackManager.shared.remove(assetPackWithID: id)
    }

    func isWanted(_ model: ModelSpecValue) -> Bool {
        model.required || (UserDefaults.standard.object(forKey: "want:\(model.fileName)") as? Bool ?? true)
    }

    func setWanted(_ model: ModelSpecValue, _ wanted: Bool) {
        UserDefaults.standard.set(wanted, forKey: "want:\(model.fileName)")
        if !wanted && !model.required, let url = try? directory.appending(path: model.fileName) {
            try? FileManager.default.removeItem(at: url)
        }
    }

    /// The required models are present; see `TranscriptionModel.modelsReady`.
    func isReady() -> Bool {
        catalog.filter(\.required).allSatisfy(isInstalled)
    }

    enum Outcome: Sendable {
        case installed(ModelSpecValue)
        case alreadyInstalled(ModelSpecValue)
        case notAModel(String)
        case failed(String, String)
    }

    /// Copies one picked file in, verifying it. Blocking: call off the main actor.
    func importFile(_ source: URL, progress: @Sendable (String, Double) -> Void) -> Outcome {
        let name = source.lastPathComponent
        // Files from the importer are security-scoped: readable only between
        // these two calls.
        let scoped = source.startAccessingSecurityScopedResource()
        defer { if scoped { source.stopAccessingSecurityScopedResource() } }

        let size = (try? source.resourceValues(forKeys: [.fileSizeKey]).fileSize).map(Int64.init)
        let candidates = catalog.filter { size == nil || $0.sizeBytes == size }
        guard !candidates.isEmpty else { return .notAModel(name) }
        if candidates.count == 1, isInstalled(candidates[0]) { return .alreadyInstalled(candidates[0]) }

        do {
            let directory = try directory
            let partial = directory.appending(path: ".import-\(UUID().uuidString).partial")
            FileManager.default.createFile(atPath: partial.path(percentEncoded: false), contents: nil)
            defer { try? FileManager.default.removeItem(at: partial) }

            let input = try FileHandle(forReadingFrom: source)
            let output = try FileHandle(forWritingTo: partial)
            defer { try? input.close(); try? output.close() }

            var hasher = SHA256()
            var copied: Int64 = 0
            let total = Double(size ?? candidates.map(\.sizeBytes).max() ?? 1)
            var reported = -1
            while let chunk = try input.read(upToCount: 1 << 20), !chunk.isEmpty {
                hasher.update(data: chunk)
                try output.write(contentsOf: chunk)
                copied += Int64(chunk.count)
                let percent = Int(Double(copied) / total * 100)
                if percent != reported {
                    reported = percent
                    progress(name, Double(percent) / 100)
                }
            }
            try output.close()

            let hash = hasher.finalize().map { String(format: "%02x", $0) }.joined()
            guard let model = catalog.first(where: { $0.sha256 == hash && $0.sizeBytes == copied }) else {
                return .notAModel(name)
            }

            var target = directory.appending(path: model.fileName)
            try? FileManager.default.removeItem(at: target)
            try FileManager.default.moveItem(at: partial, to: target)
            // Re-downloadable, and over a gigabyte: never in an iCloud backup.
            var values = URLResourceValues()
            values.isExcludedFromBackup = true
            try? target.setResourceValues(values)
            return .installed(model)
        } catch {
            return .failed(name, error.localizedDescription)
        }
    }

    /// Leftovers of an import cut short by the app being killed.
    func sweepPartials() {
        guard let directory = try? directory,
              let names = try? FileManager.default.contentsOfDirectory(atPath: directory.path(percentEncoded: false))
        else { return }
        for name in names where name.hasPrefix(".import-") {
            try? FileManager.default.removeItem(at: directory.appending(path: name))
        }
    }
}
