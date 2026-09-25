import AppKit

/// One borderless panel per screen during a capture session.
///
/// Lifecycle: init shows the SelectionView in its frozen phase; arm()
/// supplies the snapshot/canvas and starts live interaction. The
/// GLOBAL mouse monitor (cross-screen and foreign-app drags) is owned
/// by CaptureSessionController, which broadcasts events to every
/// screen's SelectionView — routing them through just the primary
/// screen's window would leave the other displays dead whenever
/// another app holds focus.
final class OverlayWindow: NSPanel {
    let selectionView: SelectionView

    private let onCancel: () -> Void
    private var onConfirm: (() -> Void)?
    private var onSaveKey: (() -> Void)?
    private var onOcr: (() -> Void)?
    private var onPin: (() -> Void)?

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
        // Frozen phase: click-through. A hit-testable full-screen panel
        // ordered above every window becomes "the window under the
        // cursor" in the WindowServer's eyes — the previously hovered
        // window gets a mouse-exited and its hover popup (CSS :hover,
        // tracking-area-driven overlays) collapses BEFORE the async
        // SCKit snapshot runs, so the popup never makes it into the
        // frozen image. NonactivatingPanel protects the KEYBOARD focus
        // only; hit-testing is a separate channel. arm() flips this
        // back once the snapshot is frozen and hover no longer matters.
        ignoresMouseEvents = true
        acceptsMouseMovedEvents = true
        collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .ignoresCycle]
        isReleasedWhenClosed = false
        contentView = selectionView
        selectionView.onCancel = onCancel
        // Faded in by the session controller after ordering front.
        alphaValue = 0
    }

    /// Switches the view to live phase and wires the points where
    /// keyboard shortcuts and the toolbar leave the window.
    func arm(
        canvas: Canvas,
        display: DisplayContext,
        onConfirm: @escaping () -> Void,
        onSave: @escaping () -> Void,
        onOcr: @escaping () -> Void,
        onPin: @escaping () -> Void,
    ) {
        // Snapshot is frozen — live interaction begins, so the panel
        // must become hit-testable again (see init's ignoresMouseEvents
        // note). The real desktop's hover popups may now collapse, but
        // their image is already in the snapshot the user is looking at.
        ignoresMouseEvents = false
        self.onConfirm = onConfirm
        self.onOcr = onOcr
        self.onPin = onPin
        onSaveKey = onSave
        selectionView.onConfirm = onConfirm
        selectionView.onSave = onSave
        selectionView.onOcr = onOcr
        selectionView.onPin = onPin
        selectionView.onCancel = onCancel
        selectionView.arm(canvas: canvas, display: display)
    }

    override var canBecomeKey: Bool {
        true
    }

    override var canBecomeMain: Bool {
        false
    }

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
            case "p":
                if selectionView.currentSelection != nil {
                    onPin?()
                }
            case "z":
                selectionView.undoAnnotation()
            default:
                super.keyDown(with: event)
            }
        }
    }
}
