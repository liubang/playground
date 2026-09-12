import AppKit

/// Owns one capture session's lifecycle (design doc §5):
/// permission gate → instant frozen overlays → SCScreenshotManager
/// snapshot (excluding our overlays) → live selection → confirm →
/// output → teardown and focus restoration.
///
/// Failure policy: if the snapshot stage throws, the session stays in
/// frozen phase (user can still Esc out) rather than leaving the
/// desktop covered by inert black windows that look like a hang.
@MainActor
final class CaptureSessionController {
    private enum OutputAction { case copy, save }

    private var windows: [OverlayWindow] = []
    private var displays: [DisplayContext] = []
    private var previousApp: NSRunningApplication?
    private(set) var isActive = false
    /// Global ESC monitor, installed only for the frozen phase: activation
    /// is deferred to arm() so .transient popovers (e.g. AuraBar's) survive
    /// into the snapshot — but that leaves our nonactivating overlays
    /// without key status until then, and Esc-to-cancel would be dead in
    /// the gap. Removed on arm() and teardown.
    private var frozenEscMonitor: Any?

    /// Starts a capture session. No-op while one is already up.
    func begin() {
        guard !isActive else { return }
        guard CapturePermissions.hasAccess() else {
            CapturePermissions.showGuidance()
            return
        }
        isActive = true

        // Remember who owned the keyboard so teardown can give it back —
        // AuraShot is an accessory app and would otherwise leave the
        // user focused on nothing.
        previousApp = NSWorkspace.shared.frontmostApplication

        // Instant feedback first: frozen overlays cover every screen
        // immediately (design §5). The overlays are .nonactivatingPanel,
        // so ordering them front doesn't resign any key window —
        // .transient popovers (AuraBar, Spotlight-style tools) stay on
        // screen, giving the candidate enumeration and the snapshot below
        // a chance to freeze them. Activation is deferred to arm(): an
        // NSApp.activate here would steal focus from the popover's owner
        // and the popover would vanish before it could be captured.
        for screen in NSScreen.screens {
            let window = OverlayWindow(screen: screen) { [weak self] in
                self?.cancel()
            }
            windows.append(window)
            window.orderFrontRegardless()
        }

        installFrozenEscMonitor()

        // Suction candidates are captured before the snapshot stage:
        // our own overlays are already filtered by PID in
        // WindowGeometry, and their geometry doubles as the exclusion
        // list for SCKit compositing.
        let overlayNumbers = Set(windows.map { Int($0.windowNumber) })
        let candidates = WindowGeometry.magnetCandidates()

        Task { @MainActor [weak self] in
            do {
                let snapshots = try await ScreenCapture.snapshotAllDisplays(
                    excludingWindowNumbers: overlayNumbers,
                )
                self?.arm(snapshots: snapshots, candidates: candidates)
            } catch {
                NSLog("AuraShot: snapshot failed: \(error.localizedDescription) — staying frozen")
            }
        }
    }

    /// Dismisses the session without producing output.
    func cancel() {
        teardown()
    }

    // MARK: - Arming

    private func arm(snapshots: [ScreenSnapshot], candidates: [WindowCandidate]) {
        guard isActive else { return }

        var displays: [DisplayContext] = []
        for screen in NSScreen.screens {
            guard let number = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? CGDirectDisplayID,
                  let snapshot = snapshots.first(where: { $0.displayID == number }) else { continue }
            displays.append(DisplayContext(screen: screen, frameNS: screen.frame, snapshot: snapshot))
        }
        // A display we couldn't snapshot would leave a hole in the
        // canvas — safer to abort the session than to half-cover it.
        guard displays.count == windows.count, !displays.isEmpty else {
            NSLog("AuraShot: display/snapshot mismatch (\(displays.count) vs \(windows.count))")
            teardown()
            return
        }

        self.displays = displays

        // Snapshot is frozen: now it's safe to take focus (this closes
        // any .transient popovers on the real desktop, but their image is
        // already in the snapshot the user is looking at).
        stopFrozenEscMonitor()
        NSApp.activate(ignoringOtherApps: true)
        windows.first?.makeKeyAndOrderFront(nil)

        let canvas = Canvas(displays: displays, windowCandidates: candidates)
        for (index, window) in windows.enumerated() {
            let display = displays[index]
            window.arm(
                canvas: canvas,
                display: display,
                isPrimaryScreen: index == 0,
                onConfirm: { [weak self] in
                    self?.finish(in: display, action: .copy)
                },
                onSave: { [weak self] in
                    self?.finish(in: display, action: .save)
                },
            )
        }
    }

    // MARK: - Output

    /// Harvests the window's current selection + annotations and
    /// produces the final bitmap (design doc §5 output stage).
    private func finish(in display: DisplayContext, action: OutputAction) {
        guard let index = displays.firstIndex(where: { $0.frameNS == display.frameNS }),
              index < windows.count else {
            teardown()
            return
        }
        let view = windows[index].selectionView
        guard let selection = view.currentSelection,
              selection.width >= 3, selection.height >= 3 else { return }

        let crop = display.cropRectPixels(forSelectionPoints: selection)
        guard crop.width >= 2, crop.height >= 2,
              let base = display.snapshot.image.cropping(to: crop) else {
            teardown()
            return
        }
        let image = ImageCompositor.composite(
            base: base,
            selectionPoints: selection,
            annotations: view.currentAnnotations,
        )
        guard let image else {
            teardown()
            return
        }
        switch action {
        case .copy:
            ClipboardWriter.write(image)
            teardown()
        case .save:
            // Save FIRST tears the session down, then shows the panel:
            // a modal NSSavePanel behind screenSaver-level overlays is
            // unreliable (z-order and key-window state both fight it).
            // The frame gap lets the overlays fully detach.
            teardown()
            DispatchQueue.main.async {
                FileSaver.save(image)
            }
        }
    }

    // MARK: - Teardown

    /// While the app isn't yet active, Esc goes to whatever app has
    /// focus — watch globally so the frozen phase remains cancellable.
    private func installFrozenEscMonitor() {
        stopFrozenEscMonitor()
        frozenEscMonitor = NSEvent.addGlobalMonitorForEvents(matching: .keyDown) { [weak self] event in
            guard event.keyCode == Carbon.KeyCode.escape else { return }
            Task { @MainActor in self?.cancel() }
        }
    }

    private func stopFrozenEscMonitor() {
        if let monitor = frozenEscMonitor {
            NSEvent.removeMonitor(monitor)
            frozenEscMonitor = nil
        }
    }

    private func teardown() {
        stopFrozenEscMonitor()
        for window in windows {
            window.invalidate()
            window.orderOut(nil)
        }
        windows.removeAll()
        displays.removeAll()
        isActive = false
        previousApp?.activate()
        previousApp = nil
    }
}
