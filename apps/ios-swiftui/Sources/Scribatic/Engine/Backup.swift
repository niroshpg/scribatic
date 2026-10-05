import CommonCrypto
import CryptoKit
import Foundation
import Security

/// Encrypted backup: every note and, if wanted, its recording, in one file the
/// user saves wherever they choose. The format is docs/backup-format.md, the
/// same byte for byte as Android's Backup.kt, so a backup restores on either.
///
/// Nothing here touches a network. The user picks the place; if that place
/// is a cloud folder, the file is unreadable there without their passphrase.
enum Backup {
    static let fileExtension = "scribatic-backup"
    static let iterations: UInt32 = 600_000
    static let chunkSize = 1 << 20
    static let minimumPassphrase = 8

    enum BackupError: LocalizedError {
        case notABackup
        case wrongPassphrase
        case damaged
        case notEnoughSpace(needed: Int64)

        var errorDescription: String? {
            switch self {
            case .notABackup: return "This isn't a Scribatic backup."
            case .wrongPassphrase: return "The passphrase doesn't open this backup."
            case .damaged: return "The backup is damaged or incomplete, so it can't be restored."
            case let .notEnoughSpace(needed):
                return "There isn't enough free storage: it needs about \(ByteCountFormatter.string(fromByteCount: needed, countStyle: .file))."
            }
        }
    }

    // MARK: - Writing

    /// Packs `database` and `recordings` into `destination`, encrypted.
    /// `progress` gets 0...1 by bytes written.
    static func write(database: URL, recordings: [URL], to destination: URL, passphrase: String,
                      progress: (Double) -> Void) throws {
        let total = ([database] + recordings).map(fileSize).reduce(0, +)
        guard FileManager.default.createFile(atPath: destination.path(percentEncoded: false), contents: nil),
              let handle = try? FileHandle(forWritingTo: destination) else {
            throw CocoaError(.fileWriteNoPermission)
        }
        do {
            defer { try? handle.close() }
            let sealed = try SealedWriter(handle: handle, passphrase: passphrase)
            var done: Int64 = 0
            try sealed.write(Data("SCRIBARC".utf8))
            for (name, file) in [("notes.sqlite", database)] + recordings.map({ ("recordings/\($0.lastPathComponent)", $0) }) {
                let length = fileSize(file)
                try sealed.write(UInt16(name.utf8.count).bigEndianBytes + Data(name.utf8) + UInt64(length).bigEndianBytes)
                let input = try FileHandle(forReadingFrom: file)
                defer { try? input.close() }
                while let data = try input.read(upToCount: chunkSize), !data.isEmpty {
                    try sealed.write(data)
                    done += Int64(data.count)
                    progress(total > 0 ? Double(done) / Double(total) : 1)
                }
            }
            try sealed.write(UInt16(0).bigEndianBytes)
            try sealed.finish()
        } catch {
            try? FileManager.default.removeItem(at: destination)
            throw error
        }
    }

    // MARK: - Reading

    /// Decrypts and unpacks `backup` into `folder`: `notes.sqlite` and
    /// `recordings/`. Throws `wrongPassphrase` before writing anything if the
    /// passphrase is wrong.
    static func unpack(_ backup: URL, passphrase: String, into folder: URL, progress: (Double) -> Void) throws {
        let handle = try FileHandle(forReadingFrom: backup)
        defer { try? handle.close() }
        let total = fileSize(backup)
        let reader = try SealedReader(handle: handle, passphrase: passphrase)
        let archive = ArchiveStream(reader: reader)
        guard try archive.read(8) == Data("SCRIBARC".utf8) else { throw BackupError.damaged }
        try FileManager.default.createDirectory(at: folder.appending(path: "recordings"), withIntermediateDirectories: true)
        while true {
            let nameLength = Int(try archive.read(2).bigEndianUInt)
            if nameLength == 0 { break }
            guard let name = String(data: try archive.read(nameLength), encoding: .utf8) else { throw BackupError.damaged }
            var remaining = Int64(try archive.read(8).bigEndianUInt)
            // Only what a backup holds, and never outside the folder.
            let file = URL(filePath: name).lastPathComponent
            let target: URL
            if name == "notes.sqlite" {
                target = folder.appending(path: "notes.sqlite")
            } else if name == "recordings/\(file)", file.hasPrefix("recording-"), file.hasSuffix(".wav") {
                target = folder.appending(path: "recordings").appending(path: file)
            } else {
                throw BackupError.damaged
            }
            FileManager.default.createFile(atPath: target.path(percentEncoded: false), contents: nil)
            let output = try FileHandle(forWritingTo: target)
            defer { try? output.close() }
            while remaining > 0 {
                let data = try archive.read(Int(min(remaining, Int64(chunkSize))))
                try output.write(contentsOf: data)
                remaining -= Int64(data.count)
                progress(total > 0 ? Double(reader.consumed) / Double(total) : 1)
            }
        }
    }

    static func fileSize(_ url: URL) -> Int64 {
        Int64((try? url.resourceValues(forKeys: [.fileSizeKey]))?.fileSize ?? 0)
    }

    // MARK: - Envelope

    static func deriveKey(_ passphrase: String, salt: Data, iterations: UInt32) -> SymmetricKey {
        let password = passphrase.precomposedStringWithCanonicalMapping
        var derived = [UInt8](repeating: 0, count: 32)
        salt.withUnsafeBytes { saltBytes in
            _ = CCKeyDerivationPBKDF(CCPBKDFAlgorithm(kCCPBKDF2), password, password.utf8.count,
                                     saltBytes.bindMemory(to: UInt8.self).baseAddress, salt.count,
                                     CCPseudoRandomAlgorithm(kCCPRFHmacAlgSHA256), iterations,
                                     &derived, derived.count)
        }
        return SymmetricKey(data: derived)
    }

    static func header(salt: Data, prefix: Data, iterations: UInt32) -> Data {
        Data("SCRIBKUP".utf8) + Data([1, 0, 0, 0]) + iterations.bigEndianBytes + salt + prefix + Data(count: 8)
    }

    static func random(_ count: Int) -> Data {
        var bytes = [UInt8](repeating: 0, count: count)
        _ = SecRandomCopyBytes(kSecRandomDefault, count, &bytes)
        return Data(bytes)
    }

    /// Encrypts in 1 MiB chunks. A full chunk is held back until more data
    /// arrives, so the last one written is always the one marked last.
    final class SealedWriter {
        private let handle: FileHandle
        private let key: SymmetricKey
        private let header: Data
        private let prefix: Data
        private var buffer = Data()
        private var index: UInt32 = 0

        init(handle: FileHandle, passphrase: String, salt: Data = Backup.random(16), prefix: Data = Backup.random(8),
             iterations: UInt32 = Backup.iterations) throws {
            self.handle = handle
            self.prefix = prefix
            header = Backup.header(salt: salt, prefix: prefix, iterations: iterations)
            key = Backup.deriveKey(passphrase, salt: salt, iterations: iterations)
            try handle.write(contentsOf: header)
        }

        func write(_ data: Data) throws {
            buffer.append(data)
            while buffer.count > chunkSize {
                try seal(buffer.prefix(chunkSize), last: false)
                buffer = Data(buffer.dropFirst(chunkSize))
            }
        }

        func finish() throws {
            try seal(buffer, last: true)
            buffer = Data()
        }

        private func seal(_ plain: Data, last: Bool) throws {
            let nonce = try AES.GCM.Nonce(data: prefix + index.bigEndianBytes)
            let box = try AES.GCM.seal(plain, using: key, nonce: nonce, authenticating: header + Data([last ? 1 : 0]))
            let sealed = box.ciphertext + box.tag
            try handle.write(contentsOf: UInt32(sealed.count).bigEndianBytes + sealed)
            index += 1
        }
    }

    /// Decrypts chunk by chunk, reading one length ahead to know which chunk
    /// is the last.
    final class SealedReader {
        private let handle: FileHandle
        private let key: SymmetricKey
        private let header: Data
        private let prefix: Data
        private var index: UInt32 = 0
        private var nextLength: UInt32?
        private(set) var finished = false
        private(set) var consumed: Int64 = 0

        init(handle: FileHandle, passphrase: String) throws {
            self.handle = handle
            guard let header = try handle.read(upToCount: 48), header.count == 48,
                  header.prefix(8) == Data("SCRIBKUP".utf8) else { throw BackupError.notABackup }
            guard header[8] == 1 else { throw BackupError.notABackup }
            self.header = header
            let iterations = UInt32(header.subdata(in: 12..<16).bigEndianUInt)
            let salt = header.subdata(in: 16..<32)
            prefix = header.subdata(in: 32..<40)
            key = Backup.deriveKey(passphrase, salt: salt, iterations: iterations)
            consumed = 48
            nextLength = try readLength()
        }

        /// The next chunk's plaintext, or nil after the last.
        func next() throws -> Data? {
            if finished { return nil }
            guard let length = nextLength else { throw BackupError.damaged }
            guard let sealed = try handle.read(upToCount: Int(length)), sealed.count == Int(length), length >= 16 else {
                throw BackupError.damaged
            }
            consumed += 4 + Int64(length)
            nextLength = try readLength()
            let last = nextLength == nil
            do {
                let box = try AES.GCM.SealedBox(nonce: AES.GCM.Nonce(data: prefix + index.bigEndianBytes),
                                                ciphertext: sealed.dropLast(16), tag: sealed.suffix(16))
                let plain = try AES.GCM.open(box, using: key, authenticating: header + Data([last ? 1 : 0]))
                index += 1
                finished = last
                return plain
            } catch {
                // The first chunk failing is the passphrase; a later one, damage.
                throw index == 0 ? BackupError.wrongPassphrase : BackupError.damaged
            }
        }

        private func readLength() throws -> UInt32? {
            guard let bytes = try handle.read(upToCount: 4), !bytes.isEmpty else { return nil }
            guard bytes.count == 4 else { throw BackupError.damaged }
            return UInt32(bytes.bigEndianUInt)
        }
    }

    /// The archive's bytes across chunk boundaries.
    final class ArchiveStream {
        private let reader: SealedReader
        private var buffer = Data()

        init(reader: SealedReader) { self.reader = reader }

        func read(_ count: Int) throws -> Data {
            while buffer.count < count {
                guard let chunk = try reader.next() else { throw BackupError.damaged }
                buffer.append(chunk)
            }
            let out = buffer.prefix(count)
            buffer = Data(buffer.dropFirst(count))
            return Data(out)
        }
    }
}

private extension FixedWidthInteger {
    var bigEndianBytes: Data { withUnsafeBytes(of: bigEndian) { Data($0) } }
}

private extension Data {
    var bigEndianUInt: UInt64 { reduce(0) { $0 << 8 | UInt64($1) } }
}
