import AppKit
import Combine
import SwiftUI

/// Posted before a controller shows its popover; every other controller
/// closes its own, so at most one popover is open at a time.
extension Notification.Name {
    static let statusItemPopoverWillShow = Notification.Name("AuraBar.statusItemPopoverWillShow")
}

/// Hosts one module's presence in the menu bar: an AppKit NSStatusItem
/// (individually toggleable, cmd-draggable with a persistent position
/// like Stats) plus an NSPopover embedding the module's SwiftUI content.
///
/// Deliberately avoids SwiftUI's MenuBarExtra: its `isInserted` binding
/// spins the main runloop rebuilding the application menu on macOS 26
/// (100% CPU), and the status items never even appear.
///
/// Dismissal: .transient alone misses clicks that land on "windowless"
/// targets (Finder desktop, wallpaper) — the popover then needs a second
/// click to go away. And since a transient popover never really activates
/// the app, listening for deactivation doesn't fire either. The robust
/// fix is a global mouse monitor while the popover is shown: any click
/// delivered to *another* app (desktop included) closes it. Mouse global
/// monitors need no accessibility permission.
@MainActor
final class StatusItemController: NSObject {
    private let autosaveName: String
    private let visibilityKey: String
    private var statusItem: NSStatusItem?
    private let popover = NSPopover()
    private var cancellables = Set<AnyCancellable>()
    /// Global mouse-down monitor, installed only while the popover shows.
    private var clickMonitor: Any?

    /// The status item is inserted while `visibilityKey` is true in
    /// UserDefaults (absent = true) and removed when it flips to false;
    /// changes apply live via the defaults-did-change notification.
    init(autosaveName: String, visibilityKey: String, content: some View) {
        self.autosaveName = autosaveName
        self.visibilityKey = visibilityKey
        super.init()

        let host = NSHostingController(rootView: content)
        host.sizingOptions = .preferredContentSize
        popover.behavior = .transient
        popover.contentViewController = host

        NotificationCenter.default
            .publisher(for: UserDefaults.didChangeNotification)
            .sink { [weak self] _ in self?.applyVisibility() }
            .store(in: &cancellables)

        let center = NotificationCenter.default
        // Another module's popover is opening — close ours.
        center.addObserver(
            forName: .statusItemPopoverWillShow,
            object: nil,
            queue: .main,
        ) { [weak self] note in
            guard let sender = note.object as? StatusItemController, sender !== self else { return }
            Task { @MainActor in self?.closePopover() }
        }
        // Popover closed by any path (transient behavior, toggle, monitor)
        // — drop the click monitor with it.
        center.addObserver(
            forName: NSPopover.didCloseNotification,
            object: popover,
            queue: .main,
        ) { [weak self] _ in
            Task { @MainActor in self?.stopClickMonitor() }
        }
        applyVisibility()
    }

    /// Republishes a store value onto the status item button whenever it
    /// changes (clock tick, weather refresh). A no-op while the item is
    /// hidden.
    func bindLabel<P: Publisher>(
        to publisher: P,
        update: @escaping (NSStatusBarButton?, P.Output) -> Void,
    ) where P.Failure == Never {
        publisher
            .receive(on: RunLoop.main)
            .sink { [weak self] output in
                update(self?.statusItem?.button, output)
            }
            .store(in: &cancellables)
    }

    // MARK: - Visibility

    private var isVisible: Bool {
        UserDefaults.standard.object(forKey: visibilityKey) as? Bool ?? true
    }

    private func applyVisibility() {
        if isVisible, statusItem == nil {
            let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
            item.autosaveName = autosaveName
            item.button?.target = self
            item.button?.action = #selector(togglePopover)
            statusItem = item
        } else if !isVisible, let item = statusItem {
            closePopover()
            NSStatusBar.system.removeStatusItem(item)
            statusItem = nil
        }
    }

    // MARK: - Popover

    @objc private func togglePopover() {
        guard let button = statusItem?.button else { return }
        if popover.isShown {
            closePopover()
        } else {
            NotificationCenter.default.post(name: .statusItemPopoverWillShow, object: self)
            popover.show(relativeTo: button.bounds, of: button, preferredEdge: .minY)
            // Bring the popover forward so text fields can take focus.
            NSApp.activate()
            startClickMonitor()
        }
    }

    private func closePopover() {
        popover.performClose(nil)
        stopClickMonitor()
    }

    // MARK: - Click-outside dismissal

    private func startClickMonitor() {
        stopClickMonitor()
        clickMonitor = NSEvent.addGlobalMonitorForEvents(
            matching: [.leftMouseDown, .rightMouseDown],
        ) { [weak self] _ in
            Task { @MainActor in self?.closePopover() }
        }
    }

    private func stopClickMonitor() {
        if let clickMonitor {
            NSEvent.removeMonitor(clickMonitor)
            self.clickMonitor = nil
        }
    }
}
