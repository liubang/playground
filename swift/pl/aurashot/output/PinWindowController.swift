import AppKit

/// A pinned capture floating above everything else (design doc §6.6,
/// milestone M3): borderless panel, drag anywhere to move, scroll or
/// ⌘+/⌘- to zoom, Esc / ⌘W / double-click to close. Multiple pins
/// coexist; each keeps the app unactivated (nonactivating panel).
@MainActor
final class PinWindowController: NSObject, NSWindowDelegate {
    private static var pins: [PinWindowController] = []
    private static var hintShown = false

    /// Pins a finished capture. `pixelScale` is the capture's pixels-
    /// per-point factor, so the panel opens at the same physical size
    /// the selection had on screen.
    static func pin(image: CGImage, pixelScale: CGFloat) {
        let controller = PinWindowController()
        pins.append(controller)
        controller.show(image: image, pixelScale: max(pixelScale, 0.5))
        if !hintShown {
            hintShown = true
            OcrHud.toast("滚动/⌘± 缩放 · 拖动移动 · 双击或 Esc 关闭")
        }
    }

    /// Pins whatever image the general pasteboard currently holds.
    static func pinFromClipboard() {
        guard let image = NSImage(pasteboard: .general),
              let cgImage = image.cgImage(forProposedRect: nil, context: nil, hints: nil)
        else {
            OcrHud.toast("剪贴板里没有图片")
            return
        }
        pin(image: cgImage, pixelScale: NSScreen.main?.backingScaleFactor ?? 2)
    }

    private var panel: NSPanel?

    private func show(image: CGImage, pixelScale: CGFloat) {
        let view = PinImageView(image: image, pixelScale: pixelScale)
        let panel = NSPanel(
            contentRect: NSRect(origin: .zero, size: view.baseSize),
            styleMask: [.borderless, .nonactivatingPanel],
            backing: .buffered,
            defer: false,
        )
        panel.level = .floating
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = true
        panel.isReleasedWhenClosed = false
        panel.collectionBehavior = [.canJoinAllSpaces, .ignoresCycle]
        panel.delegate = self
        panel.contentView = view

        // Open centered under the mouse, capped to a sensible size.
        let mouse = NSEvent.mouseLocation
        let screen = NSScreen.screens.first { $0.frame.contains(mouse) } ?? NSScreen.main
        var frame = panel.frame
        if let screen {
            let maxSize = NSSize(width: screen.frame.width * 0.8, height: screen.frame.height * 0.8)
            if frame.width > maxSize.width || frame.height > maxSize.height {
                view.zoom = min(maxSize.width / frame.width, maxSize.height / frame.height)
                frame.size = view.currentSize
            }
            frame.origin = CGPoint(x: mouse.x - frame.width / 2, y: mouse.y - frame.height / 2)
        }
        panel.setFrame(frame, display: false)
        panel.alphaValue = 0
        panel.orderFrontRegardless()
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0.15
            panel.animator().alphaValue = 1
        }
        self.panel = panel
    }

    func windowWillClose(_: Notification) {
        panel?.delegate = nil
        panel = nil
        Self.pins.removeAll { $0 === self }
    }
}

/// The pin's interactive surface: image rendering + drag/zoom/close
/// gestures. Owns the zoom factor and resizes its window around the
/// window's center.
private final class PinImageView: NSView {
    private let image: CGImage
    private let pixelScale: CGFloat

    var zoom: CGFloat = 1 {
        didSet { needsDisplay = true }
    }

    var baseSize: NSSize {
        NSSize(width: CGFloat(image.width) / pixelScale, height: CGFloat(image.height) / pixelScale)
    }

    var currentSize: NSSize {
        NSSize(width: baseSize.width * zoom, height: baseSize.height * zoom)
    }

    init(image: CGImage, pixelScale: CGFloat) {
        self.image = image
        self.pixelScale = pixelScale
        super.init(frame: .zero)
        wantsLayer = true
        layer?.cornerRadius = 6
        layer?.masksToBounds = true
        toolTip = "滚动/⌘± 缩放 · 拖动移动 · 双击或 Esc 关闭"
    }

    @available(*, unavailable)
    required init?(coder _: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    override var acceptsFirstResponder: Bool {
        true
    }

    override func draw(_: NSRect) {
        NSImage(cgImage: image, size: bounds.size).draw(in: bounds)
    }

    override func mouseDown(with event: NSEvent) {
        window?.makeKey()
        if event.clickCount == 2 {
            window?.close()
        }
    }

    override func mouseDragged(with event: NSEvent) {
        guard let window else { return }
        var origin = window.frame.origin
        origin.x += event.deltaX
        origin.y -= event.deltaY
        window.setFrameOrigin(origin)
    }

    override func scrollWheel(with event: NSEvent) {
        applyZoom(zoom * (1 + event.scrollingDeltaY * (event.hasPreciseScrollingDeltas ? 0.01 : 0.08)))
    }

    override func keyDown(with event: NSEvent) {
        if event.keyCode == Carbon.KeyCode.escape {
            window?.close()
            return
        }
        guard event.modifierFlags.contains(.command),
              let chars = event.charactersIgnoringModifiers
        else {
            super.keyDown(with: event)
            return
        }
        switch chars {
        case "w": window?.close()
        case "=", "+": applyZoom(zoom * 1.25)
        case "-": applyZoom(zoom * 0.8)
        case "0": applyZoom(1)
        default: super.keyDown(with: event)
        }
    }

    private func applyZoom(_ newZoom: CGFloat) {
        guard let window else { return }
        zoom = min(max(newZoom, 0.1), 8)
        let size = currentSize
        let center = CGPoint(x: window.frame.midX, y: window.frame.midY)
        window.setFrame(NSRect(
            x: center.x - size.width / 2,
            y: center.y - size.height / 2,
            width: size.width,
            height: size.height,
        ), display: true)
    }
}
