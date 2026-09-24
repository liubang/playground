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

/// Exact port of the WebUI's design tokens
/// (webui/src/styles/tokens.css — Everforest dark, the WebUI's default,
/// plus its Everforest Light Medium variant). Every token is
/// appearance-adaptive: the header's theme toggle flips the window's
/// color scheme (loom.theme) and all colors re-resolve, exactly like
/// the WebUI swapping [data-theme] on the root element.
///
/// The hex pairs below mirror tokens.css one-to-one (keep in sync —
/// SyntaxHighlighter's NSColor Palette carries the same pairs).
enum Theme {
    // Surfaces
    static let bg0 = adaptive(dark: 0x1E2326, light: 0xFDF6E3)
    static let bg1 = adaptive(dark: 0x272E33, light: 0xF4F0D9)
    static let bg2 = adaptive(dark: 0x2E383C, light: 0xEFEBD4)
    static let bg3 = adaptive(dark: 0x3D484D, light: 0xE2DCC4)
    static let bubbleUser = adaptive(dark: 0x3A4148, light: 0xE6E2CC)

    // Text
    static let fg = adaptive(dark: 0xD3C6AA, light: 0x5C6A72)
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

    // Keep the custom toolbar compact and aligned with the native window controls.
    static let toolbarHeight: CGFloat = 32
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

/// Waiting-for-model indicator: the WebUI's three-dot traveling wave
/// (.block-thinking / think-wave 1.4s — opacity 0.3↔1 with a -2px rise
/// at the midpoint, staggered 0.2s per dot). Honors Reduce Motion.
struct ThinkingDots: View {
    var body: some View {
        HStack(spacing: 6) {
            WaveDot(delay: 0)
            WaveDot(delay: 0.2)
            WaveDot(delay: 0.4)
        }
        .padding(.vertical, 6)
        .accessibilityLabel("Working")
    }

    private struct WaveDot: View {
        var delay: Double
        @State private var crest = false
        @Environment(\.accessibilityReduceMotion) private var reduceMotion

        var body: some View {
            Circle()
                .fill(Theme.muted)
                .frame(width: 7, height: 7)
                .opacity(crest && !reduceMotion ? 1 : 0.3)
                .offset(y: crest && !reduceMotion ? -2 : 0)
                .onAppear {
                    guard !reduceMotion else { return }
                    withAnimation(
                        .easeInOut(duration: 0.7)
                            .repeatForever(autoreverses: true)
                            .delay(delay),
                    ) { crest = true }
                }
        }
    }
}

// MARK: - Buttons (ui.css .btn variants)

/// maze.css .maze-btn: bordered bg1 button, primary border/text on hover.
/// Shared by the maze toolbar/detail panel and the trace view.
struct MazeButton: View {
    let title: String
    var systemImage: String?
    var help: String?
    let action: () -> Void
    @State private var hovered = false

    var body: some View {
        Button(action: action) {
            HStack(spacing: 5) {
                if let systemImage {
                    Image(systemName: systemImage).font(.system(size: 10))
                }
                Text(title)
            }
            .font(.system(size: Theme.textXs))
            .foregroundStyle(hovered ? Theme.primary : Theme.fg)
            .padding(.horizontal, 10)
            .padding(.vertical, 3)
            .background(Theme.bg1, in: RoundedRectangle(cornerRadius: Theme.radiusSm))
            .overlay(RoundedRectangle(cornerRadius: Theme.radiusSm)
                .strokeBorder(hovered ? Theme.primary : Theme.bg2, lineWidth: 1))
        }
        .buttonStyle(.plain)
        .onHover { hovered = $0 }
        .help(help ?? "")
    }
}

/// maze.css .maze-search: bg1 box with a bg2 border that flips primary
/// on focus. Shared by the maze and trace toolbars.
struct MiniSearchField: View {
    let placeholder: String
    @Binding var text: String
    var width: CGFloat = 180
    /// Hosts that drive focus themselves (e.g. a keyboard shortcut)
    /// pass their own FocusState binding; otherwise the field uses an
    /// internal one.
    var externalFocus: FocusState<Bool>.Binding?
    @FocusState private var focused: Bool

    private var isFocused: Bool {
        externalFocus?.wrappedValue ?? focused
    }

    var body: some View {
        HStack(spacing: 4) {
            Image(systemName: "magnifyingglass")
                .font(.system(size: 10))
                .foregroundStyle(Theme.muted)
            TextField(placeholder, text: $text)
                .textFieldStyle(.plain)
                .font(.system(size: Theme.textXs))
                .foregroundStyle(Theme.fg)
                .focused(externalFocus ?? $focused)
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 3)
        .frame(width: width)
        .background(Theme.bg1, in: RoundedRectangle(cornerRadius: Theme.radiusSm))
        .overlay(RoundedRectangle(cornerRadius: Theme.radiusSm)
            .strokeBorder(isFocused ? Theme.primary : Theme.bg2, lineWidth: 1))
    }
}

// MARK: - Confirmation dialog

/// A destructive-confirmation request handed to ConfirmCenter.shared;
/// the topmost mounted ConfirmDialogHost renders it (ToastCenter
/// precedent), so a sheet's dialog covers the window-level one.
struct ConfirmRequest {
    let title: String
    let message: String
    let confirmTitle: String
    var cancelTitle = "Cancel"
    let action: @MainActor () -> Void
}

/// Singleton confirm-dialog state (ToastCenter precedent). Callers ask;
/// ConfirmDialogHost overlays render — but only the LAST registered,
/// i.e. topmost, host (window root + a sheet on top of it), otherwise
/// the dialog would show once per host.
@MainActor
@Observable
final class ConfirmCenter {
    static let shared = ConfirmCenter()

    private(set) var current: ConfirmRequest?

    /// Mounted hosts in appearance order; the last one wins.
    private var hostIds: [Int] = []
    private var nextHostId = 1

    /// The host that currently renders the dialog (nil = none mounted).
    var activeHostId: Int? {
        hostIds.last
    }

    func registerHost() -> Int {
        let id = nextHostId
        nextHostId += 1
        hostIds.append(id)
        return id
    }

    func unregisterHost(_ id: Int) {
        let wasActive = activeHostId == id
        hostIds.removeAll { $0 == id }
        if wasActive {
            // A host vanishing mid-dialog (its sheet closed) drops the
            // request — the context it belonged to is gone.
            current = nil
        }
    }

    func ask(_ request: ConfirmRequest) {
        current = request
    }

    func dismiss() {
        current = nil
    }
}

/// Mounts the singleton confirmation dialog; several hosts can coexist
/// (window root + a sheet on top of it) but only the topmost renders.
struct ConfirmDialogHost: View {
    var center = ConfirmCenter.shared

    @State private var hostId = 0
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        Group {
            if center.activeHostId == hostId, let request = center.current {
                ConfirmDialogView(request: request) { center.dismiss() }
            }
        }
        .animation(reduceMotion ? nil : .easeOut(duration: 0.12), value: center.current != nil)
        .onAppear { hostId = center.registerHost() }
        .onDisappear { center.unregisterHost(hostId) }
    }
}

/// In-app confirmation dialog replacing the system .confirmationDialog:
/// the system's material and typography clash with the app palette,
/// its dark-mode destructive button renders low-contrast red-on-maroon,
/// and everything was crammed into a single bold title line. Here the
/// title/body hierarchy matches the rest of the app, and the danger
/// button is solid Theme.error with onAccent text (real contrast).
///
/// Cancel is the only keyboard-exposed action (Esc / backdrop click) —
/// destructive confirms stay pointer-only, matching macOS convention.
struct ConfirmDialogView: View {
    let request: ConfirmRequest
    let dismiss: () -> Void

    @State private var confirmHovered = false
    @State private var cancelHovered = false

    var body: some View {
        ZStack {
            // Backdrop: clicking it cancels (system behavior), and it
            // swallows every hit so the content below stays inert.
            Color.black.opacity(0.35)
                .contentShape(Rectangle())
                .onTapGesture { dismiss() }

            VStack(alignment: .leading, spacing: 0) {
                Text(request.title)
                    .font(.system(size: Theme.textMd, weight: .semibold))
                    .foregroundStyle(Theme.fg)
                    .fixedSize(horizontal: false, vertical: true)
                Text(request.message)
                    .font(.system(size: Theme.textSm))
                    .foregroundStyle(Theme.muted)
                    .fixedSize(horizontal: false, vertical: true)
                    .padding(.top, 5)
                HStack(spacing: 8) {
                    Spacer(minLength: 0)
                    Button { dismiss() } label: {
                        Text(request.cancelTitle)
                            .font(.system(size: Theme.textSm, weight: .medium))
                            .foregroundStyle(Theme.fg)
                            .padding(.horizontal, 14)
                            .padding(.vertical, 5)
                            .background(
                                cancelHovered ? Theme.bg3 : Theme.bg2,
                                in: RoundedRectangle(cornerRadius: Theme.radiusSm),
                            )
                    }
                    .buttonStyle(.plain)
                    .onHover { cancelHovered = $0 }
                    Button {
                        let action = request.action
                        dismiss()
                        action()
                    } label: {
                        Text(request.confirmTitle)
                            .font(.system(size: Theme.textSm, weight: .semibold))
                            .foregroundStyle(Theme.onAccent)
                            .padding(.horizontal, 14)
                            .padding(.vertical, 5)
                            .background(
                                Theme.error.opacity(confirmHovered ? 0.85 : 1),
                                in: RoundedRectangle(cornerRadius: Theme.radiusSm),
                            )
                    }
                    .buttonStyle(.plain)
                    .onHover { confirmHovered = $0 }
                }
                .padding(.top, 16)
            }
            .padding(.horizontal, 18)
            .padding(.vertical, 16)
            .frame(width: 340)
            .background(Theme.bg1, in: RoundedRectangle(cornerRadius: Theme.radiusLg))
            .overlay(
                RoundedRectangle(cornerRadius: Theme.radiusLg)
                    .strokeBorder(Theme.bg2, lineWidth: 1),
            )
            .shadow(color: .black.opacity(0.4), radius: 16, y: 6)
            .background {
                // Esc cancels; the destructive action stays pointer-only.
                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.escape, modifiers: [])
                    .opacity(0)
                    .frame(width: 0, height: 0)
            }
        }
        .transition(.opacity)
    }
}

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
        Button(action: action) {
            label
                .font(.system(size: size))
                .foregroundStyle(
                    isEnabled ? (hovered ? Theme.fg : Theme.muted) : Theme.muted.opacity(0.4),
                )
                .padding(.horizontal, 6)
                .padding(.vertical, 4)
                .frame(minWidth: 32, minHeight: 28)
                .contentShape(Rectangle())
                .background(
                    hovered ? Theme.bg2 : Color.clear,
                    in: RoundedRectangle(cornerRadius: Theme.radiusSm),
                )
        }
        .buttonStyle(.plain)
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
            .contentShape(Rectangle())
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
            .contentShape(Rectangle())
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

// MARK: - Toasts (ui/Toast.tsx + modal.css #toasts)

/// One toast (WebUI ToastItem): error style by default, `info` mutes
/// the border; sticky toasts never auto-dismiss.
struct ToastItem: Identifiable, Equatable, Sendable {
    let id: Int
    let msg: String
    let info: Bool
    let sticky: Bool
}

/// Global toast bus (WebUI toastStore): post from anywhere; a
/// ToastHost overlay renders the stack. Over the 4-toast cap the
/// oldest drop; non-sticky toasts auto-dismiss after 5s.
///
/// Several hosts can be mounted (window root + a sheet on top of it),
/// but only the LAST registered — i.e. topmost — host renders the
/// stack; otherwise the same toast would show once per host (a sheet
/// doesn't fully cover its window, so the window host peeks out).
@MainActor
@Observable
final class ToastCenter {
    static let shared = ToastCenter()

    private(set) var items: [ToastItem] = []
    private var nextId = 1
    private static let maxToasts = 4

    /// Mounted hosts in appearance order; the last one wins.
    private var hostIds: [Int] = []
    private var nextHostId = 1

    /// The host that currently renders the stack (nil = none mounted).
    var activeHostId: Int? {
        hostIds.last
    }

    func registerHost() -> Int {
        let id = nextHostId
        nextHostId += 1
        hostIds.append(id)
        return id
    }

    func unregisterHost(_ id: Int) {
        hostIds.removeAll { $0 == id }
    }

    func post(_ msg: String, info: Bool = false, sticky: Bool = false) {
        let id = nextId
        nextId += 1
        items.append(ToastItem(id: id, msg: msg, info: info, sticky: sticky))
        if items.count > Self.maxToasts {
            items = Array(items.suffix(Self.maxToasts))
        }
        if !sticky {
            Task { [weak self] in
                try? await Task.sleep(for: .seconds(5))
                self?.dismiss(id)
            }
        }
    }

    func dismiss(_ id: Int) {
        items.removeAll { $0.id == id }
    }
}

/// #toasts: top-right overlay stack — bg1 card with an error (default)
/// or muted (info) border, text-sm, max-width 380, manually closable,
/// fadein 0.15s. Mount one per window/sheet layer that should show
/// toasts; they all read the shared ToastCenter.
struct ToastHost: View {
    var center = ToastCenter.shared
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var hostId = 0

    var body: some View {
        VStack(alignment: .trailing, spacing: 8) {
            // Only the topmost mounted host renders; the rest stay
            // empty so one toast never shows twice (window + sheet).
            if center.activeHostId == hostId {
                ForEach(center.items) { item in
                    HStack(alignment: .top, spacing: 9) {
                        Image(systemName: item.info
                            ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
                            .font(.system(size: 13))
                            .foregroundStyle(item.info ? Theme.success : Theme.error)
                            .padding(.top, 1)
                        Text(item.msg)
                            .font(.system(size: Theme.textSm))
                            .foregroundStyle(Theme.fg)
                            .fixedSize(horizontal: false, vertical: true)
                            .frame(maxWidth: .infinity, alignment: .leading)
                        Button {
                            center.dismiss(item.id)
                        } label: {
                            Image(systemName: "xmark")
                                .font(.system(size: 10, weight: .semibold))
                                .foregroundStyle(Theme.muted)
                                .frame(width: 20, height: 20)
                                .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel("关闭提示")
                    }
                    .padding(.horizontal, 14)
                    .padding(.vertical, 10)
                    .frame(maxWidth: 380)
                    // bg2, not the WebUI's bg1: the settings panel IS
                    // bg1, so a bg1 toast melts into it — bg2 keeps the
                    // elevation contrast the WebUI gets for free from
                    // its bg0 backdrop.
                    .background(Theme.bg2, in: RoundedRectangle(cornerRadius: Theme.radiusMd))
                    .overlay(
                        RoundedRectangle(cornerRadius: Theme.radiusMd)
                            .strokeBorder(
                                item.info ? Theme.success.opacity(0.45) : Theme.error.opacity(0.6),
                                lineWidth: 1,
                            ),
                    )
                    .shadow(color: .black.opacity(0.35), radius: 12, y: 4)
                    .transition(.opacity)
                }
            }
        }
        .animation(reduceMotion ? nil : .easeInOut(duration: 0.15), value: center.items)
        .onAppear { hostId = center.registerHost() }
        .onDisappear { center.unregisterHost(hostId) }
    }
}

// MARK: - Window drag surfaces (custom titlebar regions)

extension View {
    /// Marks an empty-chrome region (chat header, statusbar, landing
    /// states) as the surface that moves the window.
    ///
    /// The window is deliberately NOT movable by its background on
    /// macOS 15+: that global flag claims every mouseDown as a
    /// potential window drag, racing SwiftUI's own hit tracking — the
    /// header's sidebar/theme toggles intermittently "clicked but did
    /// nothing". With explicit drag surfaces, controls keep their
    /// clicks and only the marked empty areas drag the window. macOS
    /// 14 has no WindowDragGesture; the AppDelegate keeps the old
    /// movable-by-background behavior there as a fallback.
    func windowDragSurface() -> some View {
        modifier(WindowDragSurfaceModifier())
    }
}

private struct WindowDragSurfaceModifier: ViewModifier {
    func body(content: Content) -> some View {
        if #available(macOS 15, *) {
            content.gesture(WindowDragGesture())
        } else {
            content
        }
    }
}
