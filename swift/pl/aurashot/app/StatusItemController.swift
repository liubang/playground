import AppKit

/// The menu-bar presence: a camera-viewfinder icon with a minimal menu
/// (capture / pin / settings / quit). The global hotkey is the primary
/// trigger; the menu exists for discoverability and as a fallback.
@MainActor
final class StatusItemController: NSObject {
    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
    private let onCapture: () -> Void
    private let onOCR: () -> Void
    private let onPinClipboard: () -> Void
    private let onSettings: () -> Void
    private let onRetryHotkeys: () -> Void

    private var captureItem: NSMenuItem?
    private var ocrItem: NSMenuItem?
    private var hotkeyWarningItem: NSMenuItem?

    init(
        onCapture: @escaping () -> Void,
        onOCR: @escaping () -> Void,
        onPinClipboard: @escaping () -> Void,
        onSettings: @escaping () -> Void,
        onRetryHotkeys: @escaping () -> Void,
    ) {
        self.onCapture = onCapture
        self.onOCR = onOCR
        self.onPinClipboard = onPinClipboard
        self.onSettings = onSettings
        self.onRetryHotkeys = onRetryHotkeys
        super.init()

        // A stable autosave name lets Tahoe persist this item's
        // menubar position/visibility across launches.
        statusItem.autosaveName = "AuraShot"
        // camera.viewfinder's default metrics leave generous padding
        // inside the glyph box — bump the point size and weight so it
        // reads as large as the other menu-bar icons it sits next to.
        statusItem.button?.image = NSImage(
            systemSymbolName: "camera.viewfinder",
            accessibilityDescription: "AuraShot",
        )?.withSymbolConfiguration(.init(pointSize: 17, weight: .medium))
        statusItem.button?.image?.isTemplate = true

        let menu = NSMenu()
        // Shown only while a Carbon registration is failing (occupied
        // by another app) — design doc §6.4's visible conflict signal.
        let warning = NSMenuItem(
            title: "⚠️ 快捷键被占用，点此重试",
            action: #selector(retryHotkeysClicked),
            keyEquivalent: "",
        )
        warning.target = self
        warning.isHidden = true
        menu.addItem(warning)
        hotkeyWarningItem = warning

        // Menu key equivalents only fire while the app is active, which
        // a menu-bar app rarely is — the Carbon hotkey is the real one,
        // so the shortcut is shown in the title instead. The titles are
        // refreshed from Settings on every (re)registration.
        let capture = NSMenuItem(title: "", action: #selector(captureClicked), keyEquivalent: "")
        capture.target = self
        menu.addItem(capture)
        captureItem = capture
        let ocr = NSMenuItem(title: "", action: #selector(ocrClicked), keyEquivalent: "")
        ocr.target = self
        menu.addItem(ocr)
        ocrItem = ocr
        menu.addItem(.separator())
        let pin = NSMenuItem(title: "贴出剪贴板图片", action: #selector(pinClicked), keyEquivalent: "")
        pin.target = self
        menu.addItem(pin)
        let reveal = NSMenuItem(title: "打开截图目录", action: #selector(revealClicked), keyEquivalent: "")
        reveal.target = self
        menu.addItem(reveal)
        menu.addItem(.separator())
        let settings = NSMenuItem(title: "偏好设置…", action: #selector(settingsClicked), keyEquivalent: ",")
        settings.target = self
        menu.addItem(settings)
        menu.addItem(.separator())
        let quit = NSMenuItem(title: "退出 AuraShot", action: #selector(quitClicked), keyEquivalent: "q")
        quit.target = self
        menu.addItem(quit)
        statusItem.menu = menu

        refreshShortcutTitles()
    }

    /// Menu titles mirror the CURRENT combos from Settings — the user
    /// can rebind them, and hardcoded titles would lie after that.
    func refreshShortcutTitles() {
        captureItem?.title = "截图（\(Settings.shared.captureCombo.display)）"
        ocrItem?.title = "截图并识别文字（\(Settings.shared.ocrCombo.display)）"
    }

    /// Shows/hides the registration-failure warning row.
    func setHotkeyWarningVisible(_ visible: Bool) {
        hotkeyWarningItem?.isHidden = !visible
    }

    @objc private func captureClicked() {
        onCapture()
    }

    @objc private func settingsClicked() {
        onSettings()
    }

    @objc private func ocrClicked() {
        onOCR()
    }

    @objc private func pinClicked() {
        onPinClipboard()
    }

    @objc private func revealClicked() {
        let directory = Settings.shared.saveDirectory
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        NSWorkspace.shared.open(directory)
    }

    @objc private func retryHotkeysClicked() {
        onRetryHotkeys()
    }

    @objc private func quitClicked() {
        NSApp.terminate(nil)
    }
}
