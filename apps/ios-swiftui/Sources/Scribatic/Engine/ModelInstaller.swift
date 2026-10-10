import BackgroundAssets
import Foundation
import System

/// Finds the model files: in their Apple-hosted asset packs (ADR-011), or in
/// Application Support, where a development build has them copied by hand
/// (TESTING.md). The app makes no network requests; the system delivers the
/// packs. Nothing else offers the files, so there is no import.
struct ModelInstaller: Sendable {

    let catalog = ModelSpecValue.catalog()

    private var directory: URL {
        get throws {
            try FileManager.default.url(for: .applicationSupportDirectory, in: .userDomainMask,
                                        appropriateFor: nil, create: true)
        }
    }

    /// Present, the catalog size, and actually readable. Size, not hash:
    /// hashing gigabytes on every launch would cost seconds, and a pack's
    /// files come from the store.
    /// Readable, not just present: a size comes from metadata alone, and a
    /// model the setup screen called installed must be one the engine can open.
    func isInstalled(_ model: ModelSpecValue) -> Bool {
        fileURL(for: model) != nil
    }

    /// Where the model is, if anywhere: its Apple-hosted asset pack first
    /// (ADR-011), then a file imported into Application Support. The engine
    /// maps whichever it gets, so a pack-delivered model is never copied.
    func fileURL(for model: ModelSpecValue) -> URL? {
        // A model split across files is there only with every part; llama.cpp
        // loads the later parts from beside the first (one pack holds them all).
        guard let first = locate(model) else { return nil }
        for part in partsOf(model) where locate(part)?.deletingLastPathComponent() != first.deletingLastPathComponent() {
            return nil
        }
        return first
    }

    /// The later parts of a model split across files; empty for most.
    func partsOf(_ model: ModelSpecValue) -> [ModelSpecValue] {
        catalog.filter { $0.partOf == model.fileName }
    }

    /// One file of the catalog: in its pack, else imported into Application Support.
    private func locate(_ model: ModelSpecValue) -> URL? {
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
    /// no mock server, for instance; the setup screen then offers to try again.
    func fetchPack(_ id: String) async throws {
        let pack = try await AssetPackManager.shared.assetPack(withID: id)
        try await AssetPackManager.shared.ensureLocalAvailability(of: pack)
    }

    func removePack(_ id: String) async {
        try? await AssetPackManager.shared.remove(assetPackWithID: id)
    }

    /// Required models always are. An optional one only once the user asks
    /// for it — it is never downloaded just because the app was installed.
    /// One already on the device (an earlier build fetched it unasked) counts
    /// as wanted until switched off.
    func isWanted(_ model: ModelSpecValue) -> Bool {
        // A later part is wanted with its first part.
        if !model.partOf.isEmpty, let first = catalog.first(where: { $0.fileName == model.partOf }) {
            return isWanted(first)
        }
        return model.required || (UserDefaults.standard.object(forKey: "want:\(model.fileName)") as? Bool ?? isInstalled(model))
    }

    func setWanted(_ model: ModelSpecValue, _ wanted: Bool) {
        UserDefaults.standard.set(wanted, forKey: "want:\(model.fileName)")
        if !wanted && !model.required {
            for spec in [model] + partsOf(model) {
                if let url = try? directory.appending(path: spec.fileName) { try? FileManager.default.removeItem(at: url) }
            }
        }
    }

    /// The required models are present; see `TranscriptionModel.modelsReady`.
    func isReady() -> Bool {
        catalog.filter(\.required).allSatisfy(isInstalled)
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
