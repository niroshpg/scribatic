import SwiftUI
import UIKit

// The Scribatic design-system tokens. Same names on Android (ui/theme) and the
// site (--paper, --ink, ...), so a colour change is a one-line edit per surface.

extension Color {
    // Ground and surfaces
    static let paper = dynamic(0xF5F5F5, 0x1B1D27)
    static let surfaceRaised = dynamic(0xFFFFFF, 0x252836)
    static let fillSecondary = dynamic(0xE6E7EB, 0x2F3242)
    static let line = dynamic(0x2D3142, 0xECEEF3, lightAlpha: 0.12, darkAlpha: 0.14)
    static let lineStrong = dynamic(0x858B9B, 0x6B7185)

    // Text
    static let ink = dynamic(0x2D3142, 0xECEEF3)
    static let inkMuted = dynamic(0x4F5D75, 0xB7BFD4)
    /// Tertiary metadata at 13pt and up only: 3.5:1 on light paper.
    static let inkSoft = dynamic(0x7A8399, 0x8B93A8)

    // Tangerine
    /// Brand fill with no small text on it: the record button, the live dot, progress.
    static let accent = dynamic(0xEB6C36, 0xEB6C36)
    /// Tangerine that meets text contrast: primary buttons, links, the app tint.
    static let accentStrong = dynamic(0xB8491A, 0xF0804F)
    static let accentSoft = dynamic(0xFDEEE6, 0x3A2419)
    static let onAccent = dynamic(0xFFFFFF, 0x1B1D27)

    // Status: always beside a word or an icon
    static let success = dynamic(0x2A7D4F, 0x5CC98A)
    static let caution = dynamic(0x946000, 0xE8B04A)
    static let danger = dynamic(0xB3263E, 0xFF6B7F)

    private static let speakers: [Color] = [
        dynamic(0xB8491A, 0xF0804F), dynamic(0x1F5FBF, 0x7FB0FF), dynamic(0x2A7D4F, 0x5CC98A),
        dynamic(0x7B3FB3, 0xC59CF0), dynamic(0xB8336A, 0xF58AB3), dynamic(0x147A86, 0x5CC8D6),
        dynamic(0x4B4BC4, 0xA3A3FF), dynamic(0x7A5A3F, 0xD2B08F),
    ]

    /// A per-speaker colour. Never the only signal: the name is always written.
    static func speaker(_ index: Int32) -> Color {
        guard index >= 0 else { return .inkSoft }
        return speakers[Int(index) % speakers.count]
    }

    private static func dynamic(_ light: UInt32, _ dark: UInt32, lightAlpha: CGFloat = 1, darkAlpha: CGFloat = 1) -> Color {
        Color(uiColor: UIColor { traits in
            traits.userInterfaceStyle == .dark
                ? UIColor(hex: dark, alpha: darkAlpha)
                : UIColor(hex: light, alpha: lightAlpha)
        })
    }
}

private extension UIColor {
    convenience init(hex: UInt32, alpha: CGFloat) {
        self.init(
            red: CGFloat((hex >> 16) & 0xFF) / 255,
            green: CGFloat((hex >> 8) & 0xFF) / 255,
            blue: CGFloat(hex & 0xFF) / 255,
            alpha: alpha
        )
    }
}

// MARK: - Type

/// Instrument Serif for titles only, Geist for the interface, Geist Mono for
/// numbers that change or line up. All scale with Dynamic Type.
extension Font {
    static let displayLarge = Font.custom("InstrumentSerif-Regular", size: 34, relativeTo: .largeTitle)
    static let displayMedium = Font.custom("InstrumentSerif-Regular", size: 26, relativeTo: .title)
    static let displaySmall = Font.custom("InstrumentSerif-Regular", size: 21, relativeTo: .title3)
    static let displayItalic = Font.custom("InstrumentSerif-Italic", size: 18, relativeTo: .body)
    static let uiTitle = Font.custom("Geist-SemiBold", size: 17, relativeTo: .headline)
    static let uiBody = Font.custom("Geist-Regular", size: 16, relativeTo: .body)
    static let uiBodySmall = Font.custom("Geist-Regular", size: 15, relativeTo: .subheadline)
    static let uiLabel = Font.custom("Geist-Medium", size: 14, relativeTo: .callout)
    static let uiCaption = Font.custom("Geist-Regular", size: 13, relativeTo: .footnote)
    static let eyebrow = Font.custom("GeistMono-Medium", size: 11.5, relativeTo: .caption)
    static let timestamp = Font.custom("GeistMono-Regular", size: 12.5, relativeTo: .caption)
}

enum Appearance {
    /// Navigation titles in the brand faces. UIKit appearance proxies are the
    /// only way to reach the large title's font from SwiftUI.
    @MainActor
    static func apply() {
        let large = UIFont(name: "InstrumentSerif-Regular", size: 34) ?? .preferredFont(forTextStyle: .largeTitle)
        let inline = UIFont(name: "Geist-SemiBold", size: 17) ?? .preferredFont(forTextStyle: .headline)
        let appearance = UINavigationBar.appearance()
        appearance.largeTitleTextAttributes = [.font: UIFontMetrics(forTextStyle: .largeTitle).scaledFont(for: large)]
        appearance.titleTextAttributes = [.font: UIFontMetrics(forTextStyle: .headline).scaledFont(for: inline)]
    }
}

// MARK: - Buttons

/// One primary action per screen: accent-strong fill, on-accent label.
struct PrimaryButtonStyle: ButtonStyle {
    @Environment(\.isEnabled) private var isEnabled

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.uiTitle)
            .foregroundStyle(Color.onAccent)
            .padding(.horizontal, 22)
            .frame(minHeight: 50)
            .background(Color.accentStrong, in: RoundedRectangle(cornerRadius: 12))
            .opacity(isEnabled ? (configuration.isPressed ? 0.85 : 1) : 0.38)
    }
}

/// Alternatives beside a primary: fill-secondary ground, ink label.
struct SecondaryButtonStyle: ButtonStyle {
    @Environment(\.isEnabled) private var isEnabled

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.uiTitle)
            .foregroundStyle(Color.ink)
            .padding(.horizontal, 22)
            .frame(minHeight: 50)
            .background(Color.fillSecondary, in: RoundedRectangle(cornerRadius: 12))
            .opacity(isEnabled ? (configuration.isPressed ? 0.8 : 1) : 0.38)
    }
}

/// The round record / stop control: the one element that uses the brand
/// accent as a fill. Stop shows a rounded square with a ring outside it.
struct RecordButton: View {
    let stop: Bool
    let label: String
    var floating = false
    let action: () -> Void
    @Environment(\.isEnabled) private var isEnabled

    var body: some View {
        Button(action: action) {
            ZStack {
                Circle().fill(Color.accent).frame(width: 72, height: 72)
                if stop {
                    RoundedRectangle(cornerRadius: 6).fill(.white).frame(width: 24, height: 24)
                } else {
                    Image(systemName: "mic.fill").font(.system(size: 30, weight: .medium)).foregroundStyle(.white)
                }
            }
            .overlay {
                if stop { Circle().stroke(Color.accent, lineWidth: 2).padding(-5) }
            }
            .shadow(color: floating ? .black.opacity(0.22) : .clear, radius: 8, y: 6)
            .frame(width: 82, height: 82)
            .contentShape(Circle())
        }
        .buttonStyle(.plain)
        .opacity(isEnabled ? 1 : 0.38)
        .accessibilityLabel(label)
    }
}

/// Pause / Resume beside the record control: a 56pt fill-secondary disc.
struct RoundButton: View {
    let systemImage: String
    let label: String
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: systemImage)
                .font(.system(size: 24, weight: .semibold))
                .foregroundStyle(Color.ink)
                .frame(width: 56, height: 56)
                .background(Color.fillSecondary, in: Circle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(label)
    }
}

/// A control with its word underneath: the recorder is where a mistaken tap costs most.
struct LabelledControl<Content: View>: View {
    let label: String
    @ViewBuilder let content: Content

    var body: some View {
        VStack(spacing: 6) {
            content
            Text(label).font(.uiLabel).foregroundStyle(Color.inkMuted).accessibilityHidden(true)
        }
    }
}

// MARK: - Status and metadata

/// The recorder's phase as a word with a dot. The word is the signal.
struct StatusPill: View {
    let phase: TranscriptionModel.Phase
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var dim = false

    var body: some View {
        HStack(spacing: 8) {
            if case .processing = phase {
                ProgressView().controlSize(.mini).tint(Color.accent)
            } else {
                Circle()
                    .fill(dotColor)
                    .frame(width: 8, height: 8)
                    .opacity(pulses && dim ? 0.35 : 1)
                    .animation(pulses ? .easeInOut(duration: 0.6).repeatForever(autoreverses: true) : .default, value: dim)
                    .onAppear { dim = true }
            }
            Text(phase.label).font(.uiLabel).foregroundStyle(textColor)
        }
        .padding(.horizontal, 12)
        .frame(minHeight: 30)
        .background(ground, in: Capsule())
    }

    private var pulses: Bool { phase == .recording && !reduceMotion }

    private var ground: Color { phase == .recording ? .accentSoft : .fillSecondary }

    private var dotColor: Color {
        switch phase {
        case .recording: .accent
        case .paused: .caution
        case .failed: .danger
        default: .inkSoft
        }
    }

    private var textColor: Color {
        switch phase {
        case .recording: .accentStrong
        case .paused: .caution
        case .failed: .danger
        default: .inkMuted
        }
    }
}

/// A small fact: an icon and a short value.
struct MetaChip: View {
    let systemImage: String
    let text: String
    var color: Color = .inkMuted

    var body: some View {
        HStack(spacing: 4) {
            Image(systemName: systemImage).font(.system(size: 12, weight: .medium))
            Text(text).font(.timestamp).monospacedDigit()
        }
        .foregroundStyle(color)
    }
}

/// A status worth stopping on: "On this device".
struct MetaBadge: View {
    let systemImage: String
    let text: String

    var body: some View {
        HStack(spacing: 5) {
            Image(systemName: systemImage).font(.system(size: 12, weight: .medium))
            Text(text).font(.uiLabel)
        }
        .foregroundStyle(Color.inkMuted)
        .padding(.horizontal, 8)
        .padding(.vertical, 4)
        .background(Color.fillSecondary, in: RoundedRectangle(cornerRadius: 6))
    }
}

/// Uppercase Geist Mono label opening a list section.
struct SectionHeader: View {
    let title: String
    var trailing: String?

    var body: some View {
        HStack(alignment: .firstTextBaseline) {
            Text(title.uppercased()).font(.eyebrow).tracking(1.6).foregroundStyle(Color.inkMuted)
            Spacer()
            if let trailing {
                Text(trailing).font(.timestamp).foregroundStyle(Color.inkSoft)
            }
        }
        .accessibilityAddTraits(.isHeader)
    }
}

extension View {
    /// For a SectionHeader used as a plain List's section header: those pin
    /// to the top while scrolling, and without a ground the rows scroll
    /// visibly underneath the label.
    func pinnedHeaderGround() -> some View {
        padding(.horizontal, 16)
            .padding(.top, 14)
            .padding(.bottom, 6)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Color.paper)
            .listRowInsets(EdgeInsets())
    }
}

/// Whole-screen message: the brand mark for empty states, a warning for failures.
struct EmptyStateView<Actions: View>: View {
    let title: String
    let message: String
    var failure = false
    var aside: String?
    @ViewBuilder var actions: Actions

    var body: some View {
        ScrollView {
            VStack(spacing: 12) {
                if failure {
                    Image(systemName: "exclamationmark.triangle")
                        .font(.system(size: 40, weight: .medium))
                        .foregroundStyle(Color.danger)
                } else {
                    Image("Mark").resizable().frame(width: 60, height: 60).accessibilityHidden(true)
                }
                Text(title).font(.displayMedium).foregroundStyle(Color.ink).multilineTextAlignment(.center)
                Text(message)
                    .font(.uiBodySmall)
                    .foregroundStyle(Color.inkMuted)
                    .multilineTextAlignment(.center)
                    .frame(maxWidth: 320)
                    .textSelection(.enabled)
                if let aside {
                    Text(aside).font(.displayItalic).foregroundStyle(Color.inkSoft)
                }
                HStack(spacing: 8) { actions }.padding(.top, 8)
            }
            .padding(.horizontal, 32)
            .padding(.vertical, 24)
            .frame(maxWidth: .infinity)
        }
        .defaultScrollAnchor(.center)
        .scrollBounceBehavior(.basedOnSize)
    }
}

extension EmptyStateView where Actions == EmptyView {
    init(title: String, message: String, failure: Bool = false, aside: String? = nil) {
        self.init(title: title, message: message, failure: failure, aside: aside) { EmptyView() }
    }
}

/// One speaker: dot, name, pencil. Tapping renames.
struct SpeakerChip: View {
    let speaker: SpeakerLabelValue
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 8) {
                Circle().fill(Color.speaker(speaker.index)).frame(width: 10, height: 10)
                Text(speaker.displayName).font(.uiLabel).foregroundStyle(Color.ink)
                Image(systemName: "pencil").font(.system(size: 12, weight: .medium)).foregroundStyle(Color.inkMuted)
            }
            .padding(.leading, 12)
            .padding(.trailing, 10)
            .frame(minHeight: 40)
            .background(Color.surfaceRaised, in: Capsule())
            .overlay(Capsule().stroke(Color.lineStrong, lineWidth: 1))
        }
        .buttonStyle(.plain)
        .accessibilityHint("Gives this speaker a name")
    }
}

/// Wraps its children onto as many rows as they need.
struct FlowLayout: Layout {
    var spacing: CGFloat = 8

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let rows = arrange(subviews, width: proposal.width ?? .infinity)
        let height = rows.map(\.height).reduce(0, +) + spacing * CGFloat(max(rows.count - 1, 0))
        let width = rows.map(\.width).max() ?? 0
        return CGSize(width: proposal.width ?? width, height: height)
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        var y = bounds.minY
        for row in arrange(subviews, width: bounds.width) {
            var x = bounds.minX
            for index in row.indices {
                let size = subviews[index].sizeThatFits(.unspecified)
                subviews[index].place(at: CGPoint(x: x, y: y + (row.height - size.height) / 2), proposal: ProposedViewSize(size))
                x += size.width + spacing
            }
            y += row.height + spacing
        }
    }

    private struct Row { var indices: [Int] = []; var width: CGFloat = 0; var height: CGFloat = 0 }

    private func arrange(_ subviews: Subviews, width: CGFloat) -> [Row] {
        var rows = [Row()]
        for index in subviews.indices {
            let size = subviews[index].sizeThatFits(.unspecified)
            let needed = rows[rows.count - 1].indices.isEmpty ? size.width : rows[rows.count - 1].width + spacing + size.width
            if needed > width, !rows[rows.count - 1].indices.isEmpty {
                rows.append(Row())
            }
            var row = rows[rows.count - 1]
            row.width = row.indices.isEmpty ? size.width : row.width + spacing + size.width
            row.height = max(row.height, size.height)
            row.indices.append(index)
            rows[rows.count - 1] = row
        }
        return rows
    }
}

func timestamp(_ ms: Int64) -> String {
    Duration.milliseconds(ms).formatted(.time(pattern: .minuteSecond))
}
