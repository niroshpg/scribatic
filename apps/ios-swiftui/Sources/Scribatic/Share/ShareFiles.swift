import AVFoundation
import UIKit

/// Files made for the share sheet: a transcript as a PDF, a recording as a
/// small M4A. Each is written to a share/ folder in the temporary directory,
/// emptied before every export, so at most one shared file is ever left
/// behind and the system may clear it at will.
enum ShareFiles {

    /// A fresh, empty share/ folder; `name` cleaned up to be a file name.
    static func target(name: String, extension ext: String) throws -> URL {
        let dir = FileManager.default.temporaryDirectory.appending(path: "share", directoryHint: .isDirectory)
        try? FileManager.default.removeItem(at: dir)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let clean = name.replacingOccurrences(of: ":", with: ".")          // "2.40 am"
            .components(separatedBy: CharacterSet(charactersIn: "\\/*?\"<>|").union(.controlCharacters))
            .joined(separator: " ")
            .trimmingCharacters(in: .whitespaces)
        return dir.appending(path: "\(clean.isEmpty ? "Scribatic note" : String(clean.prefix(80))).\(ext)")
    }

    // MARK: - PDF

    /// The exported transcript as an A4 document: the title and the date line
    /// from `transcript`'s first two lines, then `summary` when there is one
    /// (it brings its own headings, "Summary:" first, in the note's
    /// language), then the transcript, each turn's "[01:23] Ana:" in bold.
    /// Speaker prefixes are only bolded for `speakerNames`, so a lecture
    /// sentence that happens to contain a colon is left alone.
    @MainActor
    static func writePDF(to url: URL, transcript: String, summary: String, speakerNames: [String]) throws {
        let lines = transcript.components(separatedBy: "\n")
        let title = lines.first ?? ""
        let meta = lines.count > 1 ? lines[1] : ""
        let paragraphs = lines.dropFirst(2).filter { !$0.trimmingCharacters(in: .whitespaces).isEmpty }

        let doc = NSMutableAttributedString()
        func add(_ text: String, size: CGFloat, bold: Bool = false, color: UIColor = .black,
                 before: CGFloat = 0, after: CGFloat = 4, boldPrefix: Int = 0) {
            let style = NSMutableParagraphStyle()
            style.paragraphSpacingBefore = before
            style.paragraphSpacing = after
            style.lineHeightMultiple = 1.1
            let font = bold ? UIFont.boldSystemFont(ofSize: size) : UIFont.systemFont(ofSize: size)
            let part = NSMutableAttributedString(string: text + "\n", attributes: [
                .font: font, .foregroundColor: color, .paragraphStyle: style,
            ])
            if boldPrefix > 0 {
                part.addAttribute(.font, value: UIFont.boldSystemFont(ofSize: size),
                                  range: NSRange(location: 0, length: boldPrefix))
            }
            doc.append(part)
        }

        add(title, size: 20, bold: true)
        add(meta, size: 10, color: .darkGray, after: 18)
        let summaryLines = summary.components(separatedBy: "\n").filter { !$0.trimmingCharacters(in: .whitespaces).isEmpty }
        if !summaryLines.isEmpty {
            for (i, line) in summaryLines.enumerated() {
                let trimmed = line.trimmingCharacters(in: .whitespaces)
                let heading = trimmed.hasSuffix(":") && !trimmed.hasPrefix("-")
                let text = trimmed.hasPrefix("- ") ? "•  " + trimmed.dropFirst(2) : trimmed
                add(text, size: heading ? 12 : 11, bold: heading, before: heading && i > 0 ? 8 : 0, after: 3)
            }
            add("Transcript", size: 14, bold: true, before: 16, after: 6)
        }
        for paragraph in paragraphs {
            var bold = 0
            var rest = Substring(paragraph)
            if rest.hasPrefix("["), let close = rest.firstIndex(of: "]") {
                let stamp = rest[...close].count + 1                      // "[01:23] "
                bold = stamp
                rest = rest.dropFirst(stamp)
            }
            if let name = speakerNames.first(where: { rest.hasPrefix("\($0): ") }) {
                bold += name.count + 2
            } else if let match = rest.firstMatch(of: #/^Speaker \d+: /#) {
                bold += match.output.count
            }
            // NSRange counts UTF-16; the prefix is measured in Characters.
            let prefix = NSString(string: String(paragraph.prefix(bold))).length
            add(paragraph, size: 11, after: 8, boldPrefix: prefix)
        }

        let renderer = PageRenderer()
        renderer.addPrintFormatter(UISimpleTextPrintFormatter(attributedText: doc), startingAtPageAt: 0)
        let data = NSMutableData()
        UIGraphicsBeginPDFContextToData(data, renderer.paperRect, nil)
        renderer.prepare(forDrawingPages: NSRange(location: 0, length: renderer.numberOfPages))
        for page in 0..<renderer.numberOfPages {
            UIGraphicsBeginPDFPage()
            renderer.drawPage(at: page, in: UIGraphicsGetPDFContextBounds())
        }
        UIGraphicsEndPDFContext()
        try data.write(to: url, options: .atomic)
    }

    /// A4 with 56-point margins and the page number centred at the foot.
    private final class PageRenderer: UIPrintPageRenderer {
        private let paper = CGRect(x: 0, y: 0, width: 595, height: 842)

        override var paperRect: CGRect { paper }
        override var printableRect: CGRect { paper.insetBy(dx: 56, dy: 56) }

        override init() {
            super.init()
            footerHeight = 28
        }

        override func drawFooterForPage(at pageIndex: Int, in footerRect: CGRect) {
            let label = "\(pageIndex + 1)" as NSString
            let attributes: [NSAttributedString.Key: Any] = [
                .font: UIFont.systemFont(ofSize: 9), .foregroundColor: UIColor.darkGray,
            ]
            let size = label.size(withAttributes: attributes)
            label.draw(at: CGPoint(x: footerRect.midX - size.width / 2, y: footerRect.maxY - size.height),
                       withAttributes: attributes)
        }
    }

    // MARK: - M4A

    /// Speech at 16 kHz mono needs little more: about 14 MB an hour.
    private static let aacBitRate = 32_000

    /// Encodes one of the app's recordings (WAV, 32-bit float, mono, 16 kHz)
    /// as AAC-LC in an M4A, with the system's own encoder. `progress` gets
    /// 0...1. Blocking: a few seconds per hour of audio.
    static func encodeM4A(wav: URL, to url: URL, progress: (Double) -> Void = { _ in }) throws {
        let input = try AVAudioFile(forReading: wav)
        let format = input.processingFormat
        let output = try AVAudioFile(forWriting: url, settings: [
            AVFormatIDKey: kAudioFormatMPEG4AAC,
            AVSampleRateKey: format.sampleRate,
            AVNumberOfChannelsKey: format.channelCount,
            AVEncoderBitRateKey: aacBitRate,
        ], commonFormat: format.commonFormat, interleaved: format.isInterleaved)
        guard let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: 16_384) else { return }
        let total = max(Double(input.length), 1)
        while input.framePosition < input.length {
            try input.read(into: buffer)
            if buffer.frameLength == 0 { break }
            try output.write(from: buffer)
            progress(Double(input.framePosition) / total)
        }
    }
}
