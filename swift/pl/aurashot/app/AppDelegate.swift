import AppKit

/// Wires up the app at launch: the status item, the global hotkeys from
/// Settings (re-registered live when the user edits them), the capture
/// session controller and the settings window.
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private var statusItemController: StatusItemController?
    private let hotKeyManager = HotKeyManager.shared
    private let sessionController = CaptureSessionController()

    private var captureAction: (() -> Void)?
    private var ocrAction: (() -> Void)?

    func applicationDidFinishLaunching(_: Notification) {
        captureAction = { [weak self] in
            Task { @MainActor in
                self?.sessionController.begin(ocr: false)
            }
        }
        ocrAction = { [weak self] in
            Task { @MainActor in
                self?.sessionController.begin(ocr: true)
            }
        }

        statusItemController = StatusItemController(
            onCapture: { [weak self] in self?.captureAction?() },
            onOCR: { [weak self] in self?.ocrAction?() },
            onPinClipboard: { PinWindowController.pinFromClipboard() },
            onSettings: { SettingsWindowController.shared.show() },
            onRetryHotkeys: { [weak self] in self?.applyHotkeys() },
        )

        // ⌘⇧X — region capture; ⌘⇧O — capture + OCR. Both combos come
        // from Settings and are re-registered live on edits. The hotkey
        // recorder suspends registrations while it listens; resume
        // re-applies whatever Settings holds at that point.
        hotKeyManager.onResumeNeeded = { [weak self] in
            Task { @MainActor in self?.applyHotkeys() }
        }
        applyHotkeys()
        Settings.shared.onHotkeysChanged = { [weak self] in
            self?.applyHotkeys()
        }
    }

    private func applyHotkeys() {
        guard let captureAction, let ocrAction else { return }
        hotKeyManager.unregisterAll()
        let settings = Settings.shared
        let registeredCapture = hotKeyManager.register(
            keyCode: settings.captureCombo.keyCode,
            carbonModifiers: settings.captureCombo.carbonModifiers,
            action: captureAction,
        )
        let registeredOCR = hotKeyManager.register(
            keyCode: settings.ocrCombo.keyCode,
            carbonModifiers: settings.ocrCombo.carbonModifiers,
            action: ocrAction,
        )
        if !registeredCapture || !registeredOCR {
            NSLog("AuraShot: hotkey registration failed (capture: \(registeredCapture), ocr: \(registeredOCR)) — occupied by another app?")
        }
        // Design §6.4: surface conflicts in the menu bar, not just the
        // console — a failed registration is otherwise indistinguishable
        // from "the app is dead" to the user.
        statusItemController?.setHotkeyWarningVisible(!registeredCapture || !registeredOCR)
        statusItemController?.refreshShortcutTitles()
    }
}
