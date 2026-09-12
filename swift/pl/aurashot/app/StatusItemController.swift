import AppKit

/// The menu-bar presence: a camera-viewfinder icon with a minimal menu
/// (capture / quit). The global hotkey is the primary trigger; the menu
/// exists for discoverability and as a fallback.
@MainActor
final class StatusItemController: NSObject {
    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
    private let onCapture: () -> Void
    private let onSettings: () -> Void

    init(onCapture: @escaping () -> Void, onSettings: @escaping () -> Void) {
        self.onCapture = onCapture
        self.onSettings = onSettings
        super.init()

        // A stable autosave name lets Tahoe persist this item's
        // menubar position/visibility across launches.
        statusItem.autosaveName = "AuraShot"
        statusItem.button?.image = NSImage(
            systemSymbolName: "camera.viewfinder",
            accessibilityDescription: "AuraShot",
        )
        statusItem.button?.image?.isTemplate = true

        let menu = NSMenu()
        // Menu key equivalents only fire while the app is active, which
        // a menu-bar app rarely is — the Carbon hotkey is the real one,
        // so the shortcut is shown in the title instead.
        let capture = NSMenuItem(title: "截图（⌘⇧X）", action: #selector(captureClicked), keyEquivalent: "")
        capture.target = self
        menu.addItem(capture)
        // OCR 选字在 M4 接入 PaddleOCR-VL 后启用；常驻占位避免菜单结构变动。
        let ocr = NSMenuItem(title: "截图并识别文字（⌘⇧O，即将推出）", action: nil, keyEquivalent: "")
        ocr.isEnabled = false
        menu.addItem(ocr)
        menu.addItem(.separator())
        let settings = NSMenuItem(title: "偏好设置…", action: #selector(settingsClicked), keyEquivalent: ",")
        settings.target = self
        menu.addItem(settings)
        menu.addItem(.separator())
        let quit = NSMenuItem(title: "退出 AuraShot", action: #selector(quitClicked), keyEquivalent: "q")
        quit.target = self
        menu.addItem(quit)
        statusItem.menu = menu
    }

    @objc private func captureClicked() {
        onCapture()
    }

    @objc private func settingsClicked() {
        onSettings()
    }

    @objc private func quitClicked() {
        NSApp.terminate(nil)
    }
}
