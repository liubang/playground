import AppKit
import SwiftUI

/// Owns the standalone settings window. Created lazily on first use and
/// kept alive afterwards (close = hide), so reopening is instant and
/// keeps the selected tab.
@MainActor
final class SettingsWindowController {
    static let shared = SettingsWindowController()

    private var window: NSWindow?

    /// The fixed SwiftUI content size; the window's frame is the content
    /// rect plus the titlebar (~32pt on macOS 26, measured as 640×432).
    private static let contentSize = NSSize(width: 640, height: 400)

    func show() {
        if window == nil {
            let window = NSWindow(
                contentRect: NSRect(origin: .zero, size: Self.contentSize),
                styleMask: [.titled, .closable, .miniaturizable],
                backing: .buffered,
                defer: false,
            )
            window.title = "AuraBar 设置"
            // Let our themed content run flush under a transparent
            // titlebar, System Settings style.
            window.titlebarAppearsTransparent = true
            window.titleVisibility = .hidden
            window.contentViewController = NSHostingController(rootView: SettingsView())
            window.isReleasedWhenClosed = false
            self.window = window
        }
        guard let window else { return }
        // Center BEFORE ordering front. On the very first show the
        // freshly created window reports a degenerate frame (0×titlebar
        // — the WindowServer slot isn't realized until ordering), which
        // produced x = midX − 0 and parked the window at the screen
        // edge; every later show has a real frame and centered fine.
        // Deriving the frame from the known content size sidesteps that.
        centerOnMouseScreen(window, frame: window.frameRect(forContentRect: NSRect(origin: .zero, size: Self.contentSize)))
        // Activate BEFORE ordering the window, and one runloop pass
        // later: the action fires while the status-item menu is still
        // being torn down, which closes by re-activating the previous
        // app — an activation request made right now loses that race and
        // the window never becomes key (the "click the gear twice" bug).
        DispatchQueue.main.async {
            // Same user-activation-policy workaround as the popover —
            // see StatusItemController.togglePopover. The modern
            // activate() loses the race to the app that re-takes focus
            // when the status-item menu closes.
            NSApp.activate(ignoringOtherApps: true)
            window.makeKeyAndOrderFront(nil)
            // Belt and suspenders: re-center with the now-real frame in
            // the same runloop pass (invisible to the user), in case
            // ordering changed the actual size on a future macOS.
            self.centerOnMouseScreen(window, frame: window.frame)
        }
    }

    /// Centers the window on the screen containing the mouse cursor
    /// (visible frame, so the menu bar and Dock stay clear) — more
    /// useful than NSWindow.center() for an accessory app, which always
    /// centers on the main screen regardless of which display the user
    /// clicked the menu bar icon on. Deliberately re-centers on every
    /// show instead of using frame autosave: a settings window benefits
    /// more from being under the user's eyes than from remembering an
    /// arbitrary position.
    private func centerOnMouseScreen(_ window: NSWindow, frame: NSRect) {
        let mouse = NSEvent.mouseLocation
        let screen = NSScreen.screens.first { NSMouseInRect(mouse, $0.frame, false) } ?? NSScreen.main
        guard let screen else { return }
        let visible = screen.visibleFrame
        window.setFrameOrigin(NSPoint(
            x: visible.midX - frame.width / 2,
            y: visible.midY - frame.height / 2,
        ))
    }
}
