import AppKit

/// One borderless panel per screen during a capture session.
///
/// Lifecycle: init shows the SelectionView in its frozen phase; arm()
/// supplies the snapshot/canvas and starts live interaction plus — on
/// the primary screen — the global mouse monitor that makes
/// cross-screen and foreign-app drags work. invalidate() removes the
/// monitor; always pair it with orderOut on teardown.
final class OverlayWindow: NSPanel {
    let selectionView: SelectionView

    private let onCancel: () -> Void
    private var onConfirm: (() -> Void)?
    private var onSaveKey: (() -> Void)?
    private var onOcr: (() -> Void)?
    private var globalMonitor: Any?

    init(screen: NSScreen, onCancel: @escaping () -> Void) {
        self.onCancel = onCancel
        selectionView = SelectionView(
            frame: NSRect(origin: .zero, size: screen.frame.size),
        )
        super.init(
            contentRect: screen.frame,
            styleMask: [.borderless, .nonactivatingPanel],
            backing: .buffered,
            defer: false,
        )
        level = NSWindow.Level(rawValue: Int(CGWindowLevelForKey(.screenSaverWindow)))
        isOpaque = false
        backgroundColor = .clear
        hasShadow = false
        ignoresMouseEvents = false
        acceptsMouseMovedEvents = true
        collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .ignoresCycle]
        isReleasedWhenClosed = false
        contentView = selectionView
        selectionView.onCancel = onCancel
    }

    /// Switches the view to live phase and wires the points where
    /// keyboard shortcuts and the toolbar leave the window.
    func arm(
        canvas: Canvas,
        display: DisplayContext,
        isPrimaryScreen: Bool,
        onConfirm: @escaping () -> Void,
        onSave: @escaping () -> Void,
        onOcr: @escaping () -> Void,
    ) {
        self.onConfirm = onConfirm
        self.onOcr = onOcr
        onSaveKey = onSave
        selectionView.onConfirm = onConfirm
        selectionView.onSave = onSave
        selectionView.onOcr = onOcr
        selectionView.onCancel = onCancel
        selectionView.arm(canvas: canvas, display: display)
        if isPrimaryScreen {
            installGlobalMouseMonitor()
        }
    }

    func invalidate() {
        if let monitor = globalMonitor {
            NSEvent.removeMonitor(monitor)
            globalMonitor = nil
        }
    }

    deinit {
        invalidate()
    }

    /// Global events arrive only while OTHER apps are active — exactly
    /// the cases where AppKit won't deliver local events to our
    /// nonactivating panels (click-through screens, mid-drag across a
    /// display boundary). Each screen's view filters by containment,
    /// so only the display under the cursor reacts.
    private func installGlobalMouseMonitor() {
        let mask: NSEvent.EventTypeMask = [
            .leftMouseDown, .leftMouseDragged, .leftMouseUp, .mouseMoved,
        ]
        globalMonitor = NSEvent.addGlobalMonitorForEvents(matching: mask) { [weak self] event in
            guard let self else { return }
            let pointCG = CoordinateSpace.pointToCG(NSEvent.mouseLocation)
            selectionView.handleGlobalMouse(
                pointCG: pointCG,
                type: event.type,
                clickCount: event.clickCount,
                shift: event.modifierFlags.contains(.shift),
            )
        }
    }

    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { false }

    override func keyDown(with event: NSEvent) {
        switch event.keyCode {
        case Carbon.KeyCode.escape:
            onCancel()
        case Carbon.KeyCode.ansiReturn, Carbon.KeyCode.keypadEnter:
            if selectionView.currentSelection != nil {
                onConfirm?()
            }
        default:
            guard event.modifierFlags.contains(.command),
                  let chars = event.charactersIgnoringModifiers?.lowercased()
            else {
                super.keyDown(with: event)
                return
            }
            switch chars {
            case "c":
                if selectionView.currentSelection != nil {
                    onConfirm?()
                }
            case "s":
                if selectionView.currentSelection != nil {
                    onSaveKey?()
                }
            case "z":
                selectionView.undoAnnotation()
            default:
                super.keyDown(with: event)
            }
        }
    }
}
