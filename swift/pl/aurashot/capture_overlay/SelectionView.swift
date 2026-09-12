import AppKit

/// The interactive surface of an OverlayWindow (design doc §5, §6.3,
/// §6.5).
///
/// Phases and modes:
///   frozen — plain veil + hint label while SCScreenshotManager works.
///   live/idle — hover window preview, drag to create a selection.
///   live/editing — selection placed: 8 resize handles, drag-to-move,
///              annotation tools from the floating ToolStrip, ⌘Z undo.
///
/// Output never happens on mouse-up anymore; the user confirms via the
/// toolbar (✓ copies), ⏎, double-click inside the selection, ⌘C, or ⌘S.
///
/// All geometry here is view-local points (bottom-left origin). The
/// window routes GLOBAL mouse events (cross-screen drags and sessions
/// where another app owns focus) through handleGlobalMouse.
final class SelectionView: NSView {
    /// Xnip's selection frame blue (≈ system blue on a light base).
    private static let selectionBlue = NSColor(red: 0.04, green: 0.52, blue: 1.0, alpha: 1)

    // MARK: - Dependencies (set at arm time)

    private var canvas: Canvas?
    private var display: DisplayContext?
    private var candidateRects: [(rect: CGRect, name: String)] = []

    // MARK: - Interaction state

    private enum Phase { case frozen, live }
    private enum Mode { case idle, dragging, editing }

    private enum DragKind {
        case none
        case newSelection(anchor: CGPoint)
        case resize(handle: Handle, original: CGRect)
        case move(offset: CGPoint)
        case annotate(start: CGPoint)
    }

    private enum Handle: CaseIterable {
        case topLeft, topCenter, topRight, midLeft, midRight, bottomLeft, bottomCenter, bottomRight
    }

    private var phase: Phase = .frozen
    private var mode: Mode = .idle
    private var dragKind: DragKind = .none
    private var suppressSuction = false

    private var selection: CGRect?
    private var cursor: CGPoint?
    private var hoverCandidate: (rect: CGRect, name: String)?
    private var pendingAnnotation: Annotation?

    private(set) var annotations: [Annotation] = []
    private var activeTool: AnnotationTool?

    /// Outlets for the window/controller layer.
    var onConfirm: (() -> Void)? // copy + close
    var onSave: (() -> Void)?
    var onCancel: (() -> Void)?

    var currentSelection: CGRect? { selection }
    var currentAnnotations: [Annotation] { annotations }

    // MARK: - Subviews

    private let hint: NSTextField = {
        let label = NSTextField(labelWithString: "AuraShot · 正在捕获屏幕… · Esc 取消")
        label.alignment = .center
        label.textColor = NSColor.white.withAlphaComponent(0.85)
        label.font = .systemFont(ofSize: 14, weight: .medium)
        label.translatesAutoresizingMaskIntoConstraints = false
        return label
    }()

    private let toolbar = ToolStripView()
    private let palette = AnnotationPaletteView()
    private var textEditor: NSTextField?
    private var nextStepNumber = 1

    // MARK: - Init

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.withAlphaComponent(0.25).cgColor
        addSubview(hint)
        NSLayoutConstraint.activate([
            hint.centerXAnchor.constraint(equalTo: centerXAnchor),
            hint.topAnchor.constraint(equalTo: topAnchor, constant: frameRect.height * 0.2),
        ])

        toolbar.isHidden = true
        toolbar.onAction = { [weak self] action in self?.handleToolbar(action) }
        addSubview(toolbar)

        palette.isHidden = true
        palette.onChange = { [weak self] in
            // Live-update an open text editor; future strokes pick the
            // new attributes up at creation time.
            guard let self, let editor = textEditor else { return }
            editor.textColor = palette.currentColor
            editor.font = .boldSystemFont(ofSize: palette.currentFontSize)
            var frame = editor.frame
            frame.size.height = palette.currentFontSize * 1.6
            editor.frame = frame
        }
        addSubview(palette)
    }

    @available(*, unavailable)
    required init?(coder _: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    /// Switches from frozen to live: installs the captured background,
    /// the suction table and the cross-display canvas.
    func arm(canvas: Canvas, display: DisplayContext) {
        self.canvas = canvas
        self.display = display
        candidateRects = canvas.windowCandidates.compactMap { candidate in
            let ns = CoordinateSpace.rectToNS(candidate.frameCG)
            guard ns.intersects(display.frameNS) else { return nil }
            return (
                rect: ns.offsetBy(dx: -display.frameNS.minX, dy: -display.frameNS.minY),
                name: candidate.ownerName,
            )
        }
        hint.isHidden = true
        layer?.backgroundColor = nil
        phase = .live
        needsDisplay = true
    }

    // MARK: - Public editing API (keyboard entry points)

    func undoAnnotation() {
        guard !annotations.isEmpty else { return }
        annotations.removeLast()
        needsDisplay = true
    }

    // MARK: - Toolbar

    private func handleToolbar(_ action: ToolStripView.Action) {
        switch action {
        case .tool:
            // The toolbar toggled itself already; mirror its state and
            // show the style palette while a tool is armed.
            setTool(toolbar.activeTool)
            palette.isHidden = (activeTool == nil)
            updateToolbarPlacement()
        case .undo:
            undoAnnotation()
        case .clearAll:
            annotations.removeAll()
            needsDisplay = true
        case .cancel:
            onCancel?()
        case .save:
            onSave?()
        case .done:
            confirmSelection()
        }
    }

    private func confirmSelection() {
        guard let selection, selection.width >= 3, selection.height >= 3 else { return }
        onConfirm?()
    }

    private func updateToolbarPlacement() {
        let visible = (phase == .live && mode == .editing && selection != nil)
        toolbar.isHidden = !visible
        guard visible, let selection else { return }
        let size = toolbar.intrinsicContentSize
        var origin = CGPoint(x: selection.maxX - size.width, y: selection.minY - size.height - 8)
        if origin.y < 4 { // no room below: float above the selection
            origin.y = min(selection.maxY + 8, bounds.maxY - size.height - 4)
        }
        origin.x = min(max(origin.x, 4), bounds.maxX - size.width - 4)
        toolbar.frame = CGRect(origin: origin, size: size)

        // The palette hangs under the toolbar; if that would clip, it
        // goes above it instead.
        let paletteSize = palette.intrinsicContentSize
        var paletteOrigin = CGPoint(
            x: toolbar.frame.midX - paletteSize.width / 2,
            y: toolbar.frame.minY - paletteSize.height - 6,
        )
        if paletteOrigin.y < 4 {
            paletteOrigin.y = min(toolbar.frame.maxY + 6, bounds.maxY - paletteSize.height - 4)
        }
        paletteOrigin.x = min(max(paletteOrigin.x, 4), bounds.maxX - paletteSize.width - 4)
        palette.frame = CGRect(origin: paletteOrigin, size: paletteSize)
    }

    // MARK: - Text annotation editor

    private func stampStep(at p: CGPoint) {
        annotations.append(Annotation(
            tool: .step,
            start: p,
            end: p,
            color: palette.currentColor,
            number: nextStepNumber,
        ))
        nextStepNumber += 1
        needsDisplay = true
    }

    private func beginTextEdit(at p: CGPoint) {
        endTextEdit(commit: true)
        let fontSize = palette.currentFontSize
        let field = NSTextField(frame: CGRect(
            x: p.x, y: p.y - fontSize * 0.4, width: 180, height: fontSize * 1.6,
        ))
        field.font = .boldSystemFont(ofSize: fontSize)
        field.textColor = palette.currentColor
        field.isBezeled = false
        field.drawsBackground = true
        field.backgroundColor = NSColor.black.withAlphaComponent(0.35)
        field.focusRingType = .none
        field.target = self
        field.action = #selector(textEditCommitted(_:))
        addSubview(field)
        window?.makeFirstResponder(field)
        textEditor = field
    }

    @objc private func textEditCommitted(_: NSTextField) {
        endTextEdit(commit: true)
    }

    private func endTextEdit(commit: Bool) {
        guard let editor = textEditor else { return }
        let text = editor.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
        if commit, !text.isEmpty {
            annotations.append(Annotation(
                tool: .text,
                start: CGPoint(x: editor.frame.minX, y: editor.frame.minY + 2),
                end: editor.frame.origin,
                text: text,
                color: palette.currentColor,
                fontSize: palette.currentFontSize,
            ))
            needsDisplay = true
        }
        editor.removeFromSuperview()
        textEditor = nil
        window?.makeFirstResponder(self)
    }

    // MARK: - Local mouse events

    override func mouseDown(with event: NSEvent) {
        handleDown(at: localPoint(event), clickCount: event.clickCount)
    }

    override func mouseDragged(with event: NSEvent) {
        handleDrag(at: localPoint(event), shift: event.modifierFlags.contains(.shift))
    }

    override func mouseUp(with event: NSEvent) {
        handleUp(at: localPoint(event))
    }

    override func mouseMoved(with event: NSEvent) {
        handleMoved(at: localPoint(event))
    }

    // MARK: - Global mouse events (screens where another app is active)

    func handleGlobalMouse(
        pointCG: CGPoint,
        type: NSEvent.EventType,
        clickCount: Int,
        shift: Bool,
    ) {
        guard phase == .live, let canvas, let display else { return }
        let pointNS = CoordinateSpace.pointToNS(pointCG)
        guard canvas.display(at: pointNS)?.frameNS == display.frameNS else { return }
        let p = CGPoint(
            x: pointNS.x - display.frameNS.minX,
            y: pointNS.y - display.frameNS.minY,
        )
        switch type {
        case .leftMouseDown: handleDown(at: p, clickCount: clickCount)
        case .leftMouseDragged: handleDrag(at: p, shift: shift)
        case .leftMouseUp: handleUp(at: p)
        case .mouseMoved: handleMoved(at: p)
        default: break
        }
    }

    // MARK: - Gesture handlers

    private func handleDown(at p: CGPoint, clickCount: Int) {
        guard phase == .live else { return }
        // Global-monitor events skip hit-testing: a click that lands on
        // the toolbar or palette must not be read as "outside the
        // selection → start over".
        if !toolbar.isHidden, toolbar.frame.contains(p) { return }
        if !palette.isHidden, palette.frame.contains(p) { return }
        endTextEdit(commit: true)
        cursor = p

        switch mode {
        case .idle:
            beginNewSelection(at: p)
        case .dragging:
            break // a second button down mid-drag: ignore
        case .editing:
            guard let sel = selection else { mode = .idle; beginNewSelection(at: p); return }
            if clickCount == 2, sel.contains(effectivePoint: p), activeTool == nil {
                confirmSelection()
                return
            }
            if let handle = hitHandle(p, in: sel) {
                dragKind = .resize(handle: handle, original: sel)
            } else if let tool = activeTool, sel.contains(effectivePoint: p) {
                let point = clamp(p, to: sel)
                switch tool {
                case .text:
                    beginTextEdit(at: point)
                case .step:
                    stampStep(at: point)
                default:
                    dragKind = .annotate(start: point)
                }
            } else if sel.contains(effectivePoint: p) {
                dragKind = .move(offset: CGPoint(x: p.x - sel.minX, y: p.y - sel.minY))
            } else {
                // Clicking outside starts a fresh selection; the old
                // annotations go with the old region (cleared inside).
                beginNewSelection(at: p)
            }
        }
        needsDisplay = true
    }

    private func handleDrag(at p: CGPoint, shift: Bool) {
        guard phase == .live else { return }
        let clamped = clampToBounds(p)
        cursor = clamped

        switch dragKind {
        case .none:
            return
        case let .newSelection(anchor):
            var end = clamped
            if !shift, !suppressSuction {
                end = suctionPoint(for: end)
            }
            selection = normalizedRect(from: anchor, to: end)
        case let .resize(handle, original):
            selection = resizedRect(original: original, handle: handle, to: clamped)
        case let .move(offset):
            guard let sel = selection else { return }
            var origin = CGPoint(x: clamped.x - offset.x, y: clamped.y - offset.y)
            origin.x = min(max(origin.x, 0), bounds.maxX - sel.width)
            origin.y = min(max(origin.y, 0), bounds.maxY - sel.height)
            selection = CGRect(origin: origin, size: sel.size)
        case let .annotate(start):
            guard let tool = activeTool, let sel = selection else { return }
            let point = clamp(clamped, to: sel)
            if tool == .freehand {
                // Accumulate the stroke sample by sample.
                var stroke = pendingAnnotation ?? Annotation(
                    tool: .freehand, start: start, end: start,
                    color: palette.currentColor, lineWidth: palette.currentLineWidth,
                )
                stroke.points.append(point)
                pendingAnnotation = stroke
            } else {
                pendingAnnotation = Annotation(
                    tool: tool,
                    start: start,
                    end: point,
                    color: palette.currentColor,
                    lineWidth: palette.currentLineWidth,
                )
            }
        }
        updateToolbarPlacement()
        needsDisplay = true
    }

    private func handleUp(at p: CGPoint) {
        guard phase == .live else { return }
        let finished = dragKind
        dragKind = .none
        suppressSuction = false

        switch finished {
        case .none:
            return
        case .newSelection:
            guard let sel = selection else { mode = .idle; return }
            if sel.width >= 3, sel.height >= 3 {
                mode = .editing
            } else {
                // Click, not a drag: pick the topmost window under the
                // cursor; empty desktop clicks reset to idle.
                if let hit = candidateRects.first(where: { $0.rect.contains(p) }) {
                    selection = hit.rect.intersection(bounds)
                    mode = .editing
                } else {
                    selection = nil
                    mode = .idle
                }
            }
        case .resize, .move:
            break // stay in editing
        case let .annotate(start):
            guard var pending = pendingAnnotation else { break }
            pendingAnnotation = nil
            if pending.isSubstantial {
                if pending.tool == .mosaic, let display {
                    pending.patch = Mosaic.patch(
                        snapshot: display.snapshot.image,
                        viewRect: pending.rect,
                        display: display,
                        scale: palette.currentMosaicScale,
                    )
                }
                annotations.append(pending)
            }
            _ = start
        }
        updateToolbarPlacement()
        needsDisplay = true
    }

    private func handleMoved(at p: CGPoint) {
        guard phase == .live, case .none = dragKind else { return }
        cursor = p
        switch mode {
        case .idle:
            hoverCandidate = candidateRects.first { $0.rect.contains(p) }
            NSCursor.crosshair.set()
        case .dragging:
            break
        case .editing:
            hoverCandidate = nil
            if let sel = selection {
                if hitHandle(p, in: sel) != nil {
                    NSCursor.arrow.set()
                } else if activeTool == .text, sel.contains(effectivePoint: p) {
                    NSCursor.iBeam.set()
                } else if sel.contains(effectivePoint: p), activeTool == nil {
                    NSCursor.openHand.set()
                } else {
                    NSCursor.crosshair.set()
                }
            }
        }
        needsDisplay = true
    }

    private func beginNewSelection(at p: CGPoint) {
        mode = .dragging
        dragKind = .newSelection(anchor: p)
        // Annotations and the armed tool belong to the old region; a
        // fresh selection starts clean (Xnip behaves the same way).
        annotations.removeAll()
        nextStepNumber = 1
        setTool(nil)
        palette.isHidden = true
        // Starting a drag on top of a window usually means "a region
        // around this window", not "this window" (design §6.3).
        suppressSuction = candidateRects.contains { $0.rect.contains(p) }
        hoverCandidate = nil
        selection = CGRect(origin: p, size: .zero)
    }

    private func setTool(_ tool: AnnotationTool?) {
        activeTool = tool
        toolbar.setActiveTool(tool)
        palette.activate(tool)
    }

    // MARK: - Handles

    private func handlePoints(_ sel: CGRect) -> [(handle: Handle, point: CGPoint)] {
        [
            (.topLeft, CGPoint(x: sel.minX, y: sel.maxY)),
            (.topCenter, CGPoint(x: sel.midX, y: sel.maxY)),
            (.topRight, CGPoint(x: sel.maxX, y: sel.maxY)),
            (.midLeft, CGPoint(x: sel.minX, y: sel.midY)),
            (.midRight, CGPoint(x: sel.maxX, y: sel.midY)),
            (.bottomLeft, CGPoint(x: sel.minX, y: sel.minY)),
            (.bottomCenter, CGPoint(x: sel.midX, y: sel.minY)),
            (.bottomRight, CGPoint(x: sel.maxX, y: sel.minY)),
        ]
    }

    private func hitHandle(_ p: CGPoint, in sel: CGRect) -> Handle? {
        let hitSize: CGFloat = 14
        for (handle, point) in handlePoints(sel) {
            let rect = CGRect(x: point.x - hitSize / 2, y: point.y - hitSize / 2,
                              width: hitSize, height: hitSize)
            if rect.contains(p) { return handle }
        }
        return nil
    }

    private func resizedRect(original: CGRect, handle: Handle, to p: CGPoint) -> CGRect {
        var minX = original.minX, maxX = original.maxX
        var minY = original.minY, maxY = original.maxY
        switch handle {
        case .topLeft: minX = p.x; maxY = p.y
        case .topCenter: maxY = p.y
        case .topRight: maxX = p.x; maxY = p.y
        case .midLeft: minX = p.x
        case .midRight: maxX = p.x
        case .bottomLeft: minX = p.x; minY = p.y
        case .bottomCenter: minY = p.y
        case .bottomRight: maxX = p.x; minY = p.y
        }
        if minX > maxX { swap(&minX, &maxX) }
        if minY > maxY { swap(&minY, &maxY) }
        let rect = CGRect(x: minX, y: minY, width: maxX - minX, height: maxY - minY)
        guard rect.width >= 3, rect.height >= 3 else { return original }
        return rect.intersection(bounds)
    }

    // MARK: - Suction helpers

    private func suctionPoint(for p: CGPoint) -> CGPoint {
        let tolerance: CGFloat = 12
        var result = p
        var bestX = tolerance
        var bestY = tolerance
        for (rect, _) in candidateRects where rect.insetBy(dx: -36, dy: -36).contains(p) {
            for edgeX in [rect.minX, rect.maxX] {
                let d = abs(p.x - edgeX)
                if d < bestX { bestX = d; result.x = edgeX }
            }
            for edgeY in [rect.minY, rect.maxY] {
                let d = abs(p.y - edgeY)
                if d < bestY { bestY = d; result.y = edgeY }
            }
        }
        return clampToBounds(result)
    }

    private func clampToBounds(_ p: CGPoint) -> CGPoint {
        CGPoint(x: min(max(p.x, 0), bounds.maxX), y: min(max(p.y, 0), bounds.maxY))
    }

    private func clamp(_ p: CGPoint, to rect: CGRect) -> CGPoint {
        CGPoint(
            x: min(max(p.x, rect.minX), rect.maxX),
            y: min(max(p.y, rect.minY), rect.maxY),
        )
    }

    private func normalizedRect(from a: CGPoint, to b: CGPoint) -> CGRect {
        CGRect(x: min(a.x, b.x), y: min(a.y, b.y),
               width: abs(a.x - b.x), height: abs(a.y - b.y))
    }

    private func localPoint(_ event: NSEvent) -> CGPoint {
        convert(event.locationInWindow, from: nil)
    }

    // MARK: - Drawing

    override func draw(_: NSRect) {
        if let image = display?.snapshot.image {
            NSImage(cgImage: image, size: bounds.size).draw(in: bounds)
        } else {
            NSColor.black.withAlphaComponent(0.25).setFill()
            bounds.fill()
        }

        // Veil with a cutout for the current selection.
        let veil = NSBezierPath(rect: bounds)
        if let selection, selection.width > 0, selection.height > 0 {
            veil.append(NSBezierPath(rect: selection))
        }
        veil.windingRule = .evenOdd
        NSColor.black.withAlphaComponent(0.35).setFill()
        veil.fill()

        // Annotations on top of the revealed region.
        for annotation in annotations {
            annotation.draw()
        }
        pendingAnnotation?.draw()

        if let hoverCandidate, selection == nil {
            drawHoverPreview(hoverCandidate)
        }
        if let selection, selection.width > 0, selection.height > 0 {
            drawSelectionChrome(selection)
        }
        if phase == .live, case .none = dragKind, mode != .editing, let cursor {
            drawMagnifier(at: cursor)
        }
    }

    private func drawHoverPreview(_ candidate: (rect: CGRect, name: String)) {
        let path = NSBezierPath(rect: candidate.rect)
        path.setLineDash([6, 4], count: 2, phase: 0)
        path.lineWidth = 1
        NSColor.white.withAlphaComponent(0.9).setStroke()
        path.stroke()
        drawChip(candidate.name, at: CGPoint(
            x: candidate.rect.minX,
            y: min(candidate.rect.maxY + 6, bounds.maxY - 24),
        ))
    }

    private func drawSelectionChrome(_ sel: CGRect) {
        // Xnip-style corner-bracket frame: hairline edges plus four
        // thick, round-capped L marks at the corners. The mid-edge
        // handles stay as invisible hit zones only.
        let edge = NSBezierPath(rect: sel)
        edge.lineWidth = 1
        Self.selectionBlue.withAlphaComponent(0.9).setStroke()
        edge.stroke()

        let arm: CGFloat = 11
        Self.selectionBlue.setStroke()
        let corners: [(origin: CGPoint, dx: CGFloat, dy: CGFloat)] = [
            (CGPoint(x: sel.minX, y: sel.maxY), 1, -1), // top-left
            (CGPoint(x: sel.maxX, y: sel.maxY), -1, -1), // top-right
            (CGPoint(x: sel.minX, y: sel.minY), 1, 1), // bottom-left
            (CGPoint(x: sel.maxX, y: sel.minY), -1, 1), // bottom-right
        ]
        for (corner, dx, dy) in corners {
            let mark = NSBezierPath()
            mark.lineWidth = 3
            mark.lineCapStyle = .round
            mark.lineJoinStyle = .round
            mark.move(to: CGPoint(x: corner.x + dx * arm, y: corner.y))
            mark.line(to: corner)
            mark.line(to: CGPoint(x: corner.x, y: corner.y + dy * arm))
            mark.stroke()
        }

        let label = "\(Int(sel.width)) × \(Int(sel.height))  ·  ⏎ 复制  ⌘S 保存  Esc 取消"
        drawChip(label, at: CGPoint(
            x: sel.minX,
            y: min(sel.maxY + 6, bounds.maxY - 24),
        ))

        // Resize affordances are the corner brackets themselves
        // (Xnip draws no separate handle glyphs); hit-testing keeps
        // using the invisible 8-handle geometry.
    }

    /// Loupe: zoomed pixels around the cursor with a pixel grid,
    /// crosshair guides and a position/color readout.
    private func drawMagnifier(at p: CGPoint) {
        guard let display else { return }
        let zoom: CGFloat = 10
        let side: CGFloat = 150
        let srcHalf = side / (2 * zoom)
        let srcRect = CGRect(x: p.x - srcHalf, y: p.y - srcHalf,
                             width: srcHalf * 2, height: srcHalf * 2)
        let crop = display.cropRectPixels(forSelectionPoints: srcRect)
        guard crop.width > 0, crop.height > 0,
              let tile = display.snapshot.image.cropping(to: crop) else { return }

        var dest = CGRect(x: p.x + 24, y: p.y + 24, width: side, height: side)
        if dest.maxX > bounds.maxX - 8 { dest.origin.x = p.x - 24 - side }
        if dest.maxY > bounds.maxY - 8 { dest.origin.y = p.y - 24 - side }

        NSGraphicsContext.saveGraphicsState()
        NSBezierPath(rect: dest).addClip()
        NSGraphicsContext.current?.imageInterpolation = .none
        NSImage(cgImage: tile, size: dest.size).draw(
            in: dest, from: .zero, operation: .sourceOver, fraction: 1,
        )
        // Pixel grid.
        NSColor.white.withAlphaComponent(0.12).setStroke()
        let grid = NSBezierPath()
        grid.lineWidth = 0.5
        var gx = dest.minX + zoom
        while gx < dest.maxX {
            grid.move(to: CGPoint(x: gx, y: dest.minY))
            grid.line(to: CGPoint(x: gx, y: dest.maxY))
            gx += zoom
        }
        var gy = dest.minY + zoom
        while gy < dest.maxY {
            grid.move(to: CGPoint(x: dest.minX, y: gy))
            grid.line(to: CGPoint(x: dest.maxX, y: gy))
            gy += zoom
        }
        grid.stroke()
        // Center-pixel outline.
        let centerCell = CGRect(x: dest.midX - zoom / 2, y: dest.midY - zoom / 2,
                                width: zoom, height: zoom)
        NSColor.white.withAlphaComponent(0.9).setStroke()
        let cell = NSBezierPath(rect: centerCell)
        cell.lineWidth = 1
        cell.stroke()
        NSGraphicsContext.restoreGraphicsState()

        let border = NSBezierPath(rect: dest)
        border.lineWidth = 1
        NSColor.white.withAlphaComponent(0.9).setStroke()
        border.stroke()

        // Readout: global position + center pixel color.
        let bitmap = NSBitmapImageRep(cgImage: tile)
        let cx = min(max(Int(crop.width) / 2, 0), bitmap.pixelsWide - 1)
        let cy = min(max(Int(crop.height) / 2, 0), bitmap.pixelsHigh - 1)
        var colorText = ""
        if let color = bitmap.colorAt(x: cx, y: cy)?.usingColorSpace(.sRGB) {
            colorText = String(
                format: " #%02X%02X%02X",
                Int((color.redComponent * 255).rounded()),
                Int((color.greenComponent * 255).rounded()),
                Int((color.blueComponent * 255).rounded()),
            )
        }
        let global = display.frameNS.origin
        let readout = "X:\(Int(global.x + p.x)) Y:\(Int(global.y + p.y))\(colorText)"
        drawChip(readout, at: CGPoint(
            x: dest.minX,
            y: dest.minY > 30 ? dest.minY - 24 : dest.maxY + 6,
        ))
    }

    private func drawChip(_ text: String, at origin: CGPoint) {
        let attrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.monospacedDigitSystemFont(ofSize: 11, weight: .medium),
            .foregroundColor: NSColor.white,
        ]
        let textSize = text.size(withAttributes: attrs)
        let rect = CGRect(
            origin: origin,
            size: CGSize(width: textSize.width + 12, height: textSize.height + 7),
        )
        let chip = NSBezierPath(roundedRect: rect, xRadius: 4, yRadius: 4)
        NSColor.black.withAlphaComponent(0.72).setFill()
        chip.fill()
        text.draw(
            at: CGPoint(x: rect.minX + 6, y: rect.minY + 3.5),
            withAttributes: attrs,
        )
    }

    override func resetCursorRects() {
        addCursorRect(bounds, cursor: .crosshair)
    }

    override var acceptsFirstResponder: Bool { true }
}

private extension CGRect {
    /// contains() that tolerates the selection's 1pt chrome stroke.
    func contains(effectivePoint p: CGPoint) -> Bool {
        insetBy(dx: -1, dy: -1).contains(p)
    }
}
