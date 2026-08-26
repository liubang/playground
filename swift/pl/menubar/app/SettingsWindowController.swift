import AppKit
import SwiftUI

/// Owns the standalone settings window. Created lazily on first use and
/// kept alive afterwards (close = hide), so reopening is instant and
/// keeps the selected tab.
@MainActor
final class SettingsWindowController {
    static let shared = SettingsWindowController()

    private var window: NSWindow?

    func show() {
        if window == nil {
            let window = NSWindow(
                contentRect: NSRect(x: 0, y: 0, width: 560, height: 380),
                styleMask: [.titled, .closable, .miniaturizable],
                backing: .buffered,
                defer: false,
            )
            window.title = "AuraBar 设置"
            window.contentViewController = NSHostingController(rootView: SettingsView())
            window.setFrameAutosaveName("AuraBar.settings")
            window.isReleasedWhenClosed = false
            self.window = window
        }
        window?.center()
        window?.makeKeyAndOrderFront(nil)
        // Accessory apps can show windows; bring ours forward.
        NSApp.activate()
    }
}
