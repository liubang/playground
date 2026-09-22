// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import AppKit
import SwiftUI

/// Near-faithful port of the WebUI's design tokens
/// (webui/src/styles/tokens.css — Everforest dark, the WebUI's default,
/// plus its Everforest Light Medium variant). Every token is
/// appearance-adaptive: the header's theme toggle flips the window's
/// color scheme (loom.theme) and all colors re-resolve, exactly like
/// the WebUI swapping [data-theme] on the root element.
///
/// One deliberate deviation: the surface ladder is widened (bg0
/// deepened, bg2/bg3 lifted, fg slightly brightened). The stock
/// Everforest steps are only ~3–5% luminance apart, which flattened
/// the sidebar/main split into one murky slab on native macOS
/// rendering; the wider ladder restores depth between zones.
enum Theme {
    // Surfaces
    static let bg0 = adaptive(dark: 0x1A1F22, light: 0xFDF6E3)
    static let bg1 = adaptive(dark: 0x272E33, light: 0xF4F0D9)
    static let bg2 = adaptive(dark: 0x333D42, light: 0xECE7D0)
    static let bg3 = adaptive(dark: 0x49545A, light: 0xDDD7BC)
    static let bubbleUser = adaptive(dark: 0x3E474D, light: 0xE1DCC4)

    // Text
    static let fg = adaptive(dark: 0xDBCFB8, light: 0x5C6A72)
    static let muted = adaptive(dark: 0x9DA9A0, light: 0x5C6E5E)

    // Accents
    static let primary = adaptive(dark: 0x7FBBB3, light: 0x2273A8)
    static let success = adaptive(dark: 0xA7C080, light: 0x8DA101)
    static let info = adaptive(dark: 0x83C092, light: 0x35A77C)
    static let warning = adaptive(dark: 0xDBBC7F, light: 0xDFA000)
    static let error = adaptive(dark: 0xE67E80, light: 0xF85552)
    static let highlight = adaptive(dark: 0xE69875, light: 0xF57D26)
    static let purple = adaptive(dark: 0xD699B6, light: 0xDF69BA)
    static let onAccent = adaptive(dark: 0x1E2326, light: 0xFDF6E3)

    /// tokens.css --ring-color: the uniform focus halo (gate input /
    /// question card / pickers) — primary at 35%.
    static let ring = primary.opacity(0.35)

    // Typography (tokens.css --text-* scale)
    static let textXs: CGFloat = 11.5
    static let textSm: CGFloat = 12.5
    static let textMd: CGFloat = 13
    static let textLg: CGFloat = 14.5

    static let monoSm = Font.system(size: textSm, design: .monospaced)
    static let monoXs = Font.system(size: textXs, design: .monospaced)
    static let monoMd = Font.system(size: textMd, design: .monospaced)

    // Radii (tokens.css --radius-*)
    static let radiusSm: CGFloat = 6
    static let radiusMd: CGFloat = 8
    static let radiusLg: CGFloat = 12
    static let radiusXl: CGFloat = 16

    // Layout
    static let contentWidth: CGFloat = 960
    static let sidebarWidth: CGFloat = 288
    /// Sidebar drag-resize clamp (SidebarDivider).
    static let sidebarMinWidth: CGFloat = 220
    static let sidebarMaxWidth: CGFloat = 420
}

/// A token color resolved from the hosting view's effective appearance,
/// so flipping `loom.theme` repaints every surface without touching the
/// views (the WebUI's [data-theme] swap).
private func adaptive(dark: UInt32, light: UInt32) -> Color {
    Color(nsColor: NSColor(name: nil) { appearance in
        let hex = appearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua ? dark : light
        return NSColor(
            red: CGFloat((hex >> 16) & 0xFF) / 255,
            green: CGFloat((hex >> 8) & 0xFF) / 255,
            blue: CGFloat(hex & 0xFF) / 255,
            alpha: 1,
        )
    })
}

/// Compact relative timestamps for the sidebar meta line ("now" /
/// "3m" / "5h" / "2d" / "3w" / "4mo") — short and fixed-shape so the
/// muted second line stays quiet and never looks ragged.
func relativeTime(_ date: Date?) -> String {
    guard let date else { return "" }
    let seconds = Int(-date.timeIntervalSinceNow)
    if seconds < 60 {
        return "now"
    }
    if seconds < 3600 {
        return "\(seconds / 60)m"
    }
    if seconds < 86400 {
        return "\(seconds / 3600)h"
    }
    if seconds < 7 * 86400 {
        return "\(seconds / 86400)d"
    }
    if seconds < 30 * 86400 {
        return "\(seconds / (7 * 86400))w"
    }
    return "\(seconds / (30 * 86400))mo"
}

func formatTokenCount(_ value: Int64?) -> String {
    guard let value else { return "0" }
    if value >= 1_000_000 {
        return String(format: "%.1fM", Double(value) / 1_000_000)
    }
    if value >= 1000 {
        return String(format: "%.1fk", Double(value) / 1000)
    }
    return "\(value)"
}

/// WebUI fmtDuration: 340ms / 1.2s / 1m 05s.
func formatDuration(_ ms: Int64) -> String {
    if ms < 1000 {
        return "\(ms)ms"
    }
    let seconds = Double(ms) / 1000
    if seconds < 60 {
        return String(format: "%.1fs", seconds)
    }
    let minutes = Int(seconds) / 60
    let rest = Int(seconds) % 60
    return String(format: "%dm %02ds", minutes, rest)
}

/// WebUI fmtMsgTime: short "Aug 6 14:34" label under a message.
/// The formatter is created once — DateFormatter allocation is
/// surprisingly expensive and this runs per visible message row.
private let messageTimeFormatter: DateFormatter = {
    let formatter = DateFormatter()
    formatter.dateFormat = "MMM d HH:mm"
    return formatter
}()

func formatMessageTime(_ date: Date?) -> String {
    guard let date else { return "" }
    return messageTimeFormatter.string(from: date)
}

// MARK: - WebUI signature micro-animations

/// The WebUI's pulsing status dot (pulse 1.6s ease-in-out, 0.4…1.0).
/// Honors Reduce Motion: renders as a steady dot.
struct PulsingDot: View {
    let color: Color
    var size: CGFloat = 7
    var delay: Double = 0
    var period: Double = 1.6

    @State private var on = false
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        Circle()
            .fill(color)
            .frame(width: size, height: size)
            .opacity(on && !reduceMotion ? 0.4 : 1)
            .onAppear {
                guard !reduceMotion else { return }
                withAnimation(
                    .easeInOut(duration: period)
                        .repeatForever(autoreverses: true)
                        .delay(delay),
                ) { on = true }
            }
    }
}

/// The 1px separators the WebUI draws in --bg2 (shell hairlines).
struct Hairline: View {
    enum Axis { case horizontal, vertical }
    let axis: Axis

    var body: some View {
        switch axis {
        case .horizontal:
            Rectangle().fill(Theme.bg2).frame(height: 1).frame(maxWidth: .infinity)
        case .vertical:
            Rectangle().fill(Theme.bg2).frame(width: 1).frame(maxHeight: .infinity)
        }
    }
}

/// Badge (ui.css .badge): dot + label; the header's state/connection
/// indicators. Colors and pulse behavior follow the is-* variants.
struct BadgeView: View {
    enum Tone {
        case plain, running, awaiting, live, reconnecting, draining, dead

        var color: Color {
            switch self {
            case .plain: Theme.muted
            case .running, .live: Theme.success
            case .awaiting, .reconnecting: Theme.warning
            case .draining: Theme.highlight
            case .dead: Theme.error
            }
        }

        var pulses: Bool {
            switch self {
            case .running, .awaiting, .reconnecting: true
            default: false
            }
        }
    }

    let tone: Tone
    let text: String

    var body: some View {
        HStack(spacing: 6) {
            if tone.pulses {
                PulsingDot(color: tone.color, period: tone == .reconnecting ? 1.0 : 1.6)
            } else {
                Circle().fill(tone.color).frame(width: 7, height: 7)
            }
            Text(text)
        }
        .font(.system(size: 12))
        .foregroundStyle(tone.color)
        .fixedSize()
    }
}

/// Waiting-for-model indicator: the WebUI's three-dot traveling wave.
struct ThinkingDots: View {
    var body: some View {
        HStack(spacing: 6) {
            PulsingDot(color: Theme.muted, delay: 0)
            PulsingDot(color: Theme.muted, delay: 0.2)
            PulsingDot(color: Theme.muted, delay: 0.4)
        }
        .padding(.vertical, 6)
        .accessibilityLabel("Working")
    }
}

// MARK: - Buttons (ui.css .btn variants)

/// The WebUI's .icon-btn as a VIEW: bare muted glyph, fg + bg2 wash on
/// hover. This used to be a ButtonStyle holding @State for the hover
/// flag — but SwiftUI does not guarantee stable state storage for
/// style instances, so hover highlighting could stick or leak across
/// buttons sharing the style. State lives safely in a View instead.
struct GhostButton<Label: View>: View {
    let action: () -> Void
    /// Glyph point size — 15 in toolbars, smaller in dense footers.
    var size: CGFloat = 15
    @ViewBuilder let label: Label

    @Environment(\.isEnabled) private var isEnabled
    @State private var hovered = false

    init(size: CGFloat = 15, action: @escaping () -> Void, @ViewBuilder label: () -> Label) {
        self.action = action
        self.size = size
        self.label = label()
    }

    var body: some View {
        Button(action: action) { label }
            .buttonStyle(.plain)
            .font(.system(size: size))
            .foregroundStyle(
                isEnabled ? (hovered ? Theme.fg : Theme.muted) : Theme.muted.opacity(0.4),
            )
            .padding(.horizontal, 6)
            .padding(.vertical, 4)
            .background(
                hovered ? Theme.bg2 : Color.clear,
                in: RoundedRectangle(cornerRadius: Theme.radiusSm),
            )
            .contentShape(Rectangle())
            .onHover { hovered = $0 }
    }
}

/// .btn-primary: filled primary, on-accent label.
struct PrimaryButtonStyle: ButtonStyle {
    var tint: Color = Theme.primary
    var onTint: Color = Theme.onAccent
    @Environment(\.isEnabled) private var isEnabled

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: Theme.textMd, weight: .medium))
            .foregroundStyle(onTint)
            .padding(.horizontal, 14)
            .padding(.vertical, 7)
            .background(
                tint.opacity(configuration.isPressed ? 0.85 : 1),
                in: RoundedRectangle(cornerRadius: Theme.radiusSm),
            )
            .opacity(isEnabled ? 1 : 0.5)
    }
}

/// .btn-secondary / .btn-danger: transparent with a colored outline.
struct OutlineButtonStyle: ButtonStyle {
    var color: Color = Theme.fg
    @Environment(\.isEnabled) private var isEnabled

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: Theme.textMd, weight: .medium))
            .foregroundStyle(color)
            .padding(.horizontal, 14)
            .padding(.vertical, 7)
            .background(
                color.opacity(configuration.isPressed ? 0.12 : 0),
                in: RoundedRectangle(cornerRadius: Theme.radiusSm),
            )
            .overlay(
                RoundedRectangle(cornerRadius: Theme.radiusSm)
                    .strokeBorder(color, lineWidth: 1),
            )
            .opacity(isEnabled ? 1 : 0.5)
    }
}
