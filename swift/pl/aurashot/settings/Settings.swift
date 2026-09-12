import AppKit

/// A keyboard shortcut as Carbon sees it (key code + Carbon modifier
/// mask), with a cached display string for the UI.
struct KeyCombo: Codable, Equatable {
    var keyCode: UInt32
    var carbonModifiers: UInt32
    var display: String

    static let defaultCapture = KeyCombo(
        keyCode: Carbon.KeyCode.ansiX,
        carbonModifiers: Carbon.Modifier.cmd | Carbon.Modifier.shift,
        display: "⌘⇧X",
    )
    static let defaultOCR = KeyCombo(
        keyCode: Carbon.KeyCode.ansiO,
        carbonModifiers: Carbon.Modifier.cmd | Carbon.Modifier.shift,
        display: "⌘⇧O",
    )

    /// Builds the display string (⌃⌥⇧⌘ + key) from a recorded event.
    static func displayString(carbonModifiers: UInt32, keyCharacter: String) -> String {
        var result = ""
        if carbonModifiers & Carbon.Modifier.control != 0 { result += "⌃" }
        if carbonModifiers & Carbon.Modifier.option != 0 { result += "⌥" }
        if carbonModifiers & Carbon.Modifier.shift != 0 { result += "⇧" }
        if carbonModifiers & Carbon.Modifier.cmd != 0 { result += "⌘" }
        return result + keyCharacter.uppercased()
    }

    /// NSEvent modifier flags → Carbon mask.
    static func carbonModifiers(from flags: NSEvent.ModifierFlags) -> UInt32 {
        var mask: UInt32 = 0
        if flags.contains(.command) { mask |= Carbon.Modifier.cmd }
        if flags.contains(.shift) { mask |= Carbon.Modifier.shift }
        if flags.contains(.option) { mask |= Carbon.Modifier.option }
        if flags.contains(.control) { mask |= Carbon.Modifier.control }
        return mask
    }
}

/// UserDefaults-backed app settings. Single shared instance; hotkey
/// edits notify through onHotkeysChanged so the AppDelegate can
/// re-register with Carbon immediately.
@MainActor
final class Settings {
    static let shared = Settings()

    var onHotkeysChanged: (() -> Void)?

    private enum Key {
        static let captureCombo = "hotkey.capture"
        static let ocrCombo = "hotkey.ocr"
        static let saveDirectoryPath = "save.directoryPath"
        static let autoSave = "save.autoSave"
        static let borderShadow = "output.borderShadow"
        static let filenamePattern = "save.filenamePattern"
        static let ocrCliPath = "ocr.cliPath"
        static let ocrModelDir = "ocr.modelDir"
    }

    static let defaultFilenamePattern = "'AuraShot'-yyyyMMdd-HHmmss"

    private let defaults = UserDefaults.standard

    private init() {}

    // MARK: - Hotkeys

    var captureCombo: KeyCombo {
        get { combo(forKey: Key.captureCombo) ?? .defaultCapture }
        set { setCombo(newValue, forKey: Key.captureCombo) }
    }

    var ocrCombo: KeyCombo {
        get { combo(forKey: Key.ocrCombo) ?? .defaultOCR }
        set { setCombo(newValue, forKey: Key.ocrCombo) }
    }

    private func combo(forKey key: String) -> KeyCombo? {
        guard let data = defaults.data(forKey: key) else { return nil }
        return try? JSONDecoder().decode(KeyCombo.self, from: data)
    }

    private func setCombo(_ combo: KeyCombo, forKey key: String) {
        defaults.set(try? JSONEncoder().encode(combo), forKey: key)
        onHotkeysChanged?()
    }

    // MARK: - Save behavior

    /// Custom save folder; nil means the default ~/Pictures/AuraShot.
    var saveDirectory: URL {
        get {
            if let path = defaults.string(forKey: Key.saveDirectoryPath), !path.isEmpty {
                return URL(fileURLWithPath: path, isDirectory: true)
            }
            let pictures = FileManager.default.urls(for: .picturesDirectory, in: .userDomainMask).first
            return (pictures ?? URL(fileURLWithPath: NSHomeDirectory()))
                .appendingPathComponent("AuraShot", isDirectory: true)
        }
        set {
            defaults.set(newValue.path, forKey: Key.saveDirectoryPath)
        }
    }

    /// When true, ⌘S / the save button writes straight to saveDirectory
    /// without presenting NSSavePanel.
    var autoSave: Bool {
        get { defaults.bool(forKey: Key.autoSave) }
        set { defaults.set(newValue, forKey: Key.autoSave) }
    }

    /// Output framing (transparent padding + drop shadow +
    /// hairline border) applied to copy/save results. Defaults to ON —
    /// the key is absent until the user flips the checkbox once.
    var borderShadow: Bool {
        get { defaults.object(forKey: Key.borderShadow) as? Bool ?? true }
        set { defaults.set(newValue, forKey: Key.borderShadow) }
    }

    /// DateFormatter pattern used for output file names.
    var filenamePattern: String {
        get {
            let value = defaults.string(forKey: Key.filenamePattern) ?? Self.defaultFilenamePattern
            return value.isEmpty ? Self.defaultFilenamePattern : value
        }
        set { defaults.set(newValue, forKey: Key.filenamePattern) }
    }

    // MARK: - OCR engine paths

    /// Explicit mllm_cli path; empty = use the well-known location
    /// (~/Library/Application Support/AuraShot/mllm_cli).
    nonisolated var ocrCliPath: String {
        get { UserDefaults.standard.string(forKey: Key.ocrCliPath) ?? "" }
        set { UserDefaults.standard.set(newValue, forKey: Key.ocrCliPath) }
    }

    /// Directory holding PaddleOCR-VL-1.6-GGUF{,-mmproj}.gguf.
    nonisolated var ocrModelDir: URL {
        get {
            if let path = UserDefaults.standard.string(forKey: Key.ocrModelDir), !path.isEmpty {
                return URL(fileURLWithPath: path, isDirectory: true)
            }
            return URL(fileURLWithPath: NSHomeDirectory())
                .appendingPathComponent("models/paddleocr-vl-1.6", isDirectory: true)
        }
        set { UserDefaults.standard.set(newValue.path, forKey: Key.ocrModelDir) }
    }

    /// Renders the pattern into a concrete file name, sanitized for the
    /// filesystem. Pure function, available off the main actor.
    nonisolated static func renderFileName(pattern: String, date: Date = Date()) -> String {
        let formatter = DateFormatter()
        formatter.dateFormat = pattern
        var name = formatter.string(from: date)
        if name.isEmpty { name = "screenshot" }
        name = name.replacingOccurrences(of: "/", with: "-")
        name = name.replacingOccurrences(of: ":", with: "-")
        return name + ".png"
    }
}
