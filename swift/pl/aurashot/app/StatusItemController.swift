import AppKit

/// The menu-bar presence: a camera-viewfinder icon with a minimal menu
/// (capture / quit). The global hotkey is the primary trigger; the menu
/// exists for discoverability and as a fallback.
@MainActor
final class StatusItemController: NSObject {
    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
    private let onCapture: () -> Void
    private let onOCR: () -> Void
    private let onSettings: () -> Void

    init(onCapture: @escaping () -> Void, onOCR: @escaping () -> Void, onSettings: @escaping () -> Void) {
        self.onCapture = onCapture
        self.onOCR = onOCR
        self.onSettings = onSettings
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
        // Menu key equivalents only fire while the app is active, which
        // a menu-bar app rarely is — the Carbon hotkey is the real one,
        // so the shortcut is shown in the title instead.
        let capture = NSMenuItem(title: "截图（⌘⇧X）", action: #selector(captureClicked), keyEquivalent: "")
        capture.target = self
        menu.addItem(capture)
        let ocr = NSMenuItem(title: "截图并识别文字（⌘⇧O）", action: #selector(ocrClicked), keyEquivalent: "")
        ocr.target = self
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

    @objc private func ocrClicked() {
        onOCR()
    }

    @objc private func quitClicked() {
        NSApp.terminate(nil)
    }
}
