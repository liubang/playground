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
        if string.hasPrefix("#") {
            string.removeFirst()
        }
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

// MARK: - Community palettes

/// Bundled community color schemes, mapped onto the same 11 semantic
/// slots the UI uses. Mapping is judgment, not transcription: an
/// "accent" in another palette's language may not be its headline
/// color (e.g. Catppuccin's signature is mauve, not its teal).
///
/// Sources (all MIT): everforest-vim, catppuccin, tokyonight.nvim,
/// nordtheme, solarized, dracula, material-theme.
extension Theme {
    /// Catppuccin Mocha.
    static let catppuccinDark = Theme(
        background: Color(hex: 0x1E1E2E),
        cardBackground: Color(hex: 0x313244),
        cardBorder: Color(hex: 0x45475A),
        textPrimary: Color(hex: 0xCDD6F4),
        textSecondary: Color(hex: 0xA6ADC8),
        accent: Color(hex: 0xCBA6F7), // mauve — the palette's signature
        rest: Color(hex: 0xF38BA8),
        warning: Color(hex: 0xF9E2AF),
        orange: Color(hex: 0xFAB387),
        ok: Color(hex: 0xA6E3A1),
        aqua: Color(hex: 0x89DCEB),
    )

    /// Catppuccin Latte.
    static let catppuccinLight = Theme(
        background: Color(hex: 0xEFF1F5),
        cardBackground: Color(hex: 0xE6E9EF),
        cardBorder: Color(hex: 0xBCC0CC),
        textPrimary: Color(hex: 0x4C4F69),
        textSecondary: Color(hex: 0x6C6F85),
        accent: Color(hex: 0x8839EF),
        rest: Color(hex: 0xD20F39),
        warning: Color(hex: 0xDF8E1D),
        orange: Color(hex: 0xFE640B),
        ok: Color(hex: 0x40A02B),
        aqua: Color(hex: 0x04A5E5),
    )

    /// Tokyo Night.
    static let tokyoNightDark = Theme(
        background: Color(hex: 0x1A1B26),
        cardBackground: Color(hex: 0x24283B),
        cardBorder: Color(hex: 0x414868),
        textPrimary: Color(hex: 0xC0CAF5),
        textSecondary: Color(hex: 0x565F89),
        accent: Color(hex: 0x7AA2F7),
        rest: Color(hex: 0xF7768E),
        warning: Color(hex: 0xE0AF68),
        orange: Color(hex: 0xFF9E64),
        ok: Color(hex: 0x9ECE6A),
        aqua: Color(hex: 0x73DACA),
    )

    /// Tokyo Night Day.
    static let tokyoNightLight = Theme(
        background: Color(hex: 0xE1E2E7),
        cardBackground: Color(hex: 0xD0D5E3),
        cardBorder: Color(hex: 0xA8AECB),
        textPrimary: Color(hex: 0x3760BF),
        textSecondary: Color(hex: 0x6172B0),
        accent: Color(hex: 0x2E7DE9),
        rest: Color(hex: 0xF52A65),
        warning: Color(hex: 0x8C6C3E),
        orange: Color(hex: 0xB15C00),
        ok: Color(hex: 0x587539),
        aqua: Color(hex: 0x007197),
    )

    /// Nord (dark only — the palette has no official light variant;
    /// the secondary text is hand-tuned between nord3 and nord4).
    static let nordDark = Theme(
        background: Color(hex: 0x2E3440),
        cardBackground: Color(hex: 0x3B4252),
        cardBorder: Color(hex: 0x4C566A),
        textPrimary: Color(hex: 0xD8DEE9),
        textSecondary: Color(hex: 0x8C9BAB),
        accent: Color(hex: 0x88C0D0),
        rest: Color(hex: 0xBF616A),
        warning: Color(hex: 0xEBCB8B),
        orange: Color(hex: 0xD08770),
        ok: Color(hex: 0xA3BE8C),
        aqua: Color(hex: 0x8FBCBB),
    )

    /// Solarized Dark.
    static let solarizedDark = Theme(
        background: Color(hex: 0x002B36),
        cardBackground: Color(hex: 0x073642),
        cardBorder: Color(hex: 0x124150),
        textPrimary: Color(hex: 0x93A1A1),
        textSecondary: Color(hex: 0x586E75),
        accent: Color(hex: 0x268BD2),
        rest: Color(hex: 0xDC322F),
        warning: Color(hex: 0xB58900),
        orange: Color(hex: 0xCB4B16),
        ok: Color(hex: 0x859900),
        aqua: Color(hex: 0x2AA198),
    )

    /// Solarized Light.
    static let solarizedLight = Theme(
        background: Color(hex: 0xFDF6E3),
        cardBackground: Color(hex: 0xEEE8D5),
        cardBorder: Color(hex: 0xE0DCC7),
        textPrimary: Color(hex: 0x657B83),
        textSecondary: Color(hex: 0x93A1A1),
        accent: Color(hex: 0x268BD2),
        rest: Color(hex: 0xDC322F),
        warning: Color(hex: 0xB58900),
        orange: Color(hex: 0xCB4B16),
        ok: Color(hex: 0x859900),
        aqua: Color(hex: 0x2AA198),
    )

    /// Dracula (dark only, the palette's identity).
    static let draculaDark = Theme(
        background: Color(hex: 0x282A36),
        cardBackground: Color(hex: 0x44475A),
        cardBorder: Color(hex: 0x6272A4),
        textPrimary: Color(hex: 0xF8F8F2),
        textSecondary: Color(hex: 0x6272A4),
        accent: Color(hex: 0xBD93F9),
        rest: Color(hex: 0xFF5555),
        warning: Color(hex: 0xF1FA8C),
        orange: Color(hex: 0xFFB86C),
        ok: Color(hex: 0x50FA7B),
        aqua: Color(hex: 0x8BE9FD),
    )

    /// Material Ocean.
    static let materialDark = Theme(
        background: Color(hex: 0x0F111A),
        cardBackground: Color(hex: 0x1B1E2B),
        cardBorder: Color(hex: 0x2D3140),
        textPrimary: Color(hex: 0xA6ACCD),
        textSecondary: Color(hex: 0x676E95),
        accent: Color(hex: 0x82AAFF),
        rest: Color(hex: 0xFF5370),
        warning: Color(hex: 0xFFCB6B),
        orange: Color(hex: 0xF78C6C),
        ok: Color(hex: 0xC3E88D),
        aqua: Color(hex: 0x89DDFF),
    )

    /// Material Lighter.
    static let materialLight = Theme(
        background: Color(hex: 0xFAFAFA),
        cardBackground: Color(hex: 0xF0F0F0),
        cardBorder: Color(hex: 0xDFE4EA),
        textPrimary: Color(hex: 0x546E7A),
        textSecondary: Color(hex: 0x90A4AE),
        accent: Color(hex: 0x6182B8),
        rest: Color(hex: 0xE53935),
        warning: Color(hex: 0xF6A434),
        orange: Color(hex: 0xF76D47),
        ok: Color(hex: 0x91B859),
        aqua: Color(hex: 0x39ADB5),
    )
}

/// A bundled palette family, picked independently from the appearance
/// preference (accent color language). Each kind carries a dark
/// variant; kinds without an official light palette (Nord, Dracula)
/// fall back to their dark variant for a light-appearance request —
/// an honest dark theme beats an invented light one.
enum ThemeKind: String, CaseIterable, Sendable {
    case everforest
    case catppuccin
    case tokyoNight
    case nord
    case solarized
    case dracula
    case material

    /// UserDefaults key; every popover subscribes it for re-rendering.
    static let key = "themeKind"

    var label: String {
        switch self {
        case .everforest: "Everforest"
        case .catppuccin: "Catppuccin"
        case .tokyoNight: "Tokyo Night"
        case .nord: "Nord"
        case .solarized: "Solarized"
        case .dracula: "Dracula"
        case .material: "Material"
        }
    }

    var darkTheme: Theme {
        switch self {
        case .everforest: .dark
        case .catppuccin: .catppuccinDark
        case .tokyoNight: .tokyoNightDark
        case .nord: .nordDark
        case .solarized: .solarizedDark
        case .dracula: .draculaDark
        case .material: .materialDark
        }
    }

    /// nil for kinds with no official light palette.
    var lightTheme: Theme? {
        switch self {
        case .everforest: .light
        case .catppuccin: .catppuccinLight
        case .tokyoNight: .tokyoNightLight
        case .nord, .dracula: nil
        case .solarized: .solarizedLight
        case .material: .materialLight
        }
    }
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

    /// The 11-slot palette for `appearance`, with the user's accent
    /// override applied on top. Callers subscribe each @AppStorage key
    /// that feeds it so the popovers re-render on change.
    func theme(for colorScheme: ColorScheme, kind: ThemeKind) -> Theme {
        let wantsDark = switch self {
        case .dark: true
        case .system: colorScheme == .dark
        case .light: false
        }
        var theme = wantsDark ? kind.darkTheme : (kind.lightTheme ?? kind.darkTheme)
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
