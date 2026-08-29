import AppKit
import SwiftUI

extension Color {
    init(hex: UInt32) {
        self.init(
            red: Double((hex >> 16) & 0xFF) / 255,
            green: Double((hex >> 8) & 0xFF) / 255,
            blue: Double(hex & 0xFF) / 255,
        )
    }

    /// "#RRGGBB" or "RRGGBB"; nil on malformed input.
    init?(hexString: String) {
        var string = hexString.trimmingCharacters(in: .whitespacesAndNewlines)
        if string.hasPrefix("#") { string.removeFirst() }
        guard string.count == 6, let value = UInt32(string, radix: 16) else { return nil }
        self.init(hex: value)
    }

    /// #RRGGBB in the sRGB space, for persisting a picked Color. nil
    /// when the color can't be resolved (e.g. a semantic/system color
    /// without a fixed RGB value).
    static func hexString(of color: Color) -> String? {
        guard let resolved = NSColor(color).usingColorSpace(.sRGB) else { return nil }
        var red: CGFloat = 0
        var green: CGFloat = 0
        var blue: CGFloat = 0
        var alpha: CGFloat = 0
        resolved.getRed(&red, green: &green, blue: &blue, alpha: &alpha)
        return String(
            format: "#%02X%02X%02X",
            Int((red * 255).rounded()),
            Int((green * 255).rounded()),
            Int((blue * 255).rounded()),
        )
    }
}

/// The accent-color override persisted by the settings UI. An empty
/// string means "theme default". Applied centrally in
/// ThemePreference.theme(for:) so every popover picks it up without
/// touching call sites; top-level views just subscribe to the key.
enum AccentColor {
    static let key = "accentColorHex"

    static var override: Color? {
        guard let raw = UserDefaults.standard.string(forKey: key), !raw.isEmpty else { return nil }
        return Color(hexString: raw)
    }
}

/// Semantic color palette: Everforest Dark Hard and Everforest Light Hard,
/// following the system appearance or pinned to either side.
///
/// Color language: teal marks "today"/accent, red marks rest (holidays,
/// weekends, errors), yellow marks warnings (shifted workdays, the sun),
/// orange is a warm highlight, green marks success, aqua marks cold/wet —
/// "colors say what kind of day it is".
struct Theme: Sendable, Equatable {
    var background: Color
    var cardBackground: Color
    var cardBorder: Color
    var textPrimary: Color
    var textSecondary: Color
    /// Teal — primary accent: today, selection, interactive highlights.
    var accent: Color
    /// Red — rest days, errors, destructive actions.
    var rest: Color
    /// Yellow — shifted workdays, warnings, sunshine.
    var warning: Color
    /// Orange — warm highlight.
    var orange: Color
    /// Green — success feedback.
    var ok: Color
    /// Aqua — cold/wet accent (snow, sleet).
    var aqua: Color

    /// Everforest Dark Hard.
    static let dark = Theme(
        background: Color(hex: 0x1E2326),
        cardBackground: Color(hex: 0x272E33),
        cardBorder: Color(hex: 0x3D484D),
        textPrimary: Color(hex: 0xD3C6AA),
        textSecondary: Color(hex: 0x859289),
        accent: Color(hex: 0x7FBBB3),
        rest: Color(hex: 0xE67E80),
        warning: Color(hex: 0xDBBC7F),
        orange: Color(hex: 0xE69875),
        ok: Color(hex: 0xA7C080),
        aqua: Color(hex: 0x83C092),
    )

    /// Everforest Light Hard.
    static let light = Theme(
        background: Color(hex: 0xFDF6E3),
        cardBackground: Color(hex: 0xF4F0D9),
        cardBorder: Color(hex: 0xE0DCC7),
        textPrimary: Color(hex: 0x5C6A72),
        textSecondary: Color(hex: 0x939F91),
        accent: Color(hex: 0x3A94C5),
        rest: Color(hex: 0xF85552),
        warning: Color(hex: 0xDFA000),
        orange: Color(hex: 0xF57D26),
        ok: Color(hex: 0x8DA101),
        aqua: Color(hex: 0x35A77C),
    )
}

/// User-selectable theme mode, persisted via @AppStorage. The key is shared
/// by every module's popover so they always switch together.
enum ThemePreference: String, CaseIterable, Sendable {
    case system
    case dark
    case light

    var label: String {
        switch self {
        case .system: "跟随系统"
        case .dark: "深色"
        case .light: "浅色"
        }
    }

    func theme(for colorScheme: ColorScheme) -> Theme {
        var theme: Theme = switch self {
        case .dark: .dark
        case .light: .light
        case .system: colorScheme == .dark ? .dark : .light
        }
        if let accent = AccentColor.override {
            theme.accent = accent
        }
        return theme
    }

    /// Pinned appearance for system-rendered controls, which follow the
    /// *system* appearance rather than our custom theme. nil = follow the
    /// system.
    var pinnedColorScheme: ColorScheme? {
        switch self {
        case .dark: .dark
        case .light: .light
        case .system: nil
        }
    }
}

extension EnvironmentValues {
    @Entry var theme: Theme = .dark
}
