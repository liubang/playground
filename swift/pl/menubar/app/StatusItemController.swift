import AppKit
import Combine
import SwiftUI

/// Hosts one module's presence in the menu bar: an AppKit NSStatusItem
/// (individually toggleable, cmd-draggable with a persistent position
/// like Stats) plus an NSPopover embedding the module's SwiftUI content.
///
/// Deliberately avoids SwiftUI's MenuBarExtra: its `isInserted` binding
/// spins the main runloop rebuilding the application menu on macOS 26
/// (100% CPU), and the status items never even appear.
@MainActor
final class StatusItemController: NSObject {
    private let autosaveName: String
    private let visibilityKey: String
    private var statusItem: NSStatusItem?
    private let popover = NSPopover()
    private var cancellables = Set<AnyCancellable>()

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
            popover.performClose(nil)
            NSStatusBar.system.removeStatusItem(item)
            statusItem = nil
        }
    }

    // MARK: - Popover

    @objc private func togglePopover() {
        guard let button = statusItem?.button else { return }
        if popover.isShown {
            popover.performClose(nil)
        } else {
            popover.show(relativeTo: button.bounds, of: button, preferredEdge: .minY)
            // Bring the popover forward so text fields can take focus.
            NSApp.activate()
        }
    }
}
