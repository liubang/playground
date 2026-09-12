import AppKit

/// The second panel under the toolbar while an annotation tool is
/// active. Xnip-style, refined:
///
///   - The TOP row adapts to the active tool: stroke-width previews for
///     shapes/freehand, A-size presets for text, pixel-block previews
///     for mosaic; hidden entirely for step markers.
///   - The BOTTOM row is always the color row: generous swatches plus a
///     "+" that opens NSColorPanel for arbitrary colors.
///   - Every tool remembers its own style (color + variant) — switching
///     tools never clobbers another tool's look.
///
/// Pure custom drawing + hit-testing like the toolbar; hover and
/// selection states are drawn, not button-based.
final class AnnotationPaletteView: NSView {
    /// Per-tool style, persisted per tool while the panel lives.
    struct ToolStyle {
        var color: NSColor = .systemRed
        var lineWidth: CGFloat = AnnotationPaletteView.widths[1]
        var fontSize: CGFloat = AnnotationPaletteView.fontSizes[1]
        var mosaicLevel: Int = 1
    }

    var onChange: (() -> Void)?

    static let widths: [CGFloat] = [2, 4, 7]
    static let fontSizes: [CGFloat] = [14, 20, 28]
    /// Mosaic block multipliers relative to the auto size.
    static let mosaicScales: [CGFloat] = [0.75, 1.0, 1.5]

    static let colors: [NSColor] = [
        .white, .black,
        .systemRed, .systemOrange, .systemYellow,
        .systemGreen, .systemBlue, .systemPurple,
    ]

    private(set) var activeTool: AnnotationTool?
    private var styles: [AnnotationTool: ToolStyle] = [:]
    private var customColor: NSColor?
    private var hoverIndex: Int? // encoded: 0..2 variant, 100+ color row, 99 = "+"

    // Layout metrics (points).
    private let rowHeight: CGFloat = 30
    private let cellWidth: CGFloat = 30
    private let padding: CGFloat = 10
    private let rowGap: CGFloat = 4

    private static let accentBlue = NSColor(red: 0.04, green: 0.52, blue: 1.0, alpha: 1)

    // MARK: - Public API

    /// Shows the variant row appropriate for the tool; nil hides the
    /// panel entirely (handled by the container).
    func activate(_ tool: AnnotationTool?) {
        activeTool = tool
        hoverIndex = nil
        invalidateIntrinsicContentSize()
        needsDisplay = true
    }

    var currentColor: NSColor { style.color }
    var currentLineWidth: CGFloat { style.lineWidth }
    var currentFontSize: CGFloat { style.fontSize }
    var currentMosaicScale: CGFloat { Self.mosaicScales[style.mosaicLevel] }

    private var style: ToolStyle {
        guard let activeTool else { return ToolStyle() }
        return styles[activeTool] ?? ToolStyle()
    }

    private func mutateStyle(_ body: (inout ToolStyle) -> Void) {
        guard let activeTool else { return }
        var value = styles[activeTool] ?? ToolStyle()
        body(&value)
        styles[activeTool] = value
    }

    // MARK: - Layout

    private var hasVariantRow: Bool {
        guard let activeTool else { return false }
        return activeTool != .step
    }

    private var colorCellCount: Int { Self.colors.count + 1 } // +1 custom

    override var intrinsicContentSize: CGSize {
        let variantCells = hasVariantRow ? 3 : 0
        let rowCount = hasVariantRow ? 2 : 1
        let width = max(
            CGFloat(variantCells) * cellWidth,
            CGFloat(colorCellCount) * cellWidth,
        ) + padding * 2
        let height = padding * 2 + CGFloat(rowCount) * rowHeight
            + (rowCount == 2 ? rowGap : 0)
        return CGSize(width: width, height: height)
    }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        layer?.cornerRadius = 12
        layer?.backgroundColor = NSColor(white: 0.99, alpha: 0.98).cgColor
        let shadow = NSShadow()
        shadow.shadowColor = NSColor.black.withAlphaComponent(0.28)
        shadow.shadowOffset = NSSize(width: 0, height: -2)
        shadow.shadowBlurRadius = 12
        self.shadow = shadow
    }

    @available(*, unavailable)
    required init?(coder _: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    // MARK: - Geometry

    /// Top row sits at the top (maxY side); color row below it.
    private func variantCellCenter(_ index: Int) -> CGPoint {
        CGPoint(
            x: padding + cellWidth * (CGFloat(index) + 0.5),
            y: bounds.maxY - padding - rowHeight / 2,
        )
    }

    private func colorCellCenter(_ index: Int) -> CGPoint {
        CGPoint(
            x: padding + cellWidth * (CGFloat(index) + 0.5),
            y: bounds.minY + padding + rowHeight / 2,
        )
    }

    /// nil / variant row 0...2 / color row 100...108 (108 = custom "+").
    private func hitCell(at p: CGPoint) -> Int? {
        if hasVariantRow {
            for index in 0 ..< 3 {
                let c = variantCellCenter(index)
                if abs(p.x - c.x) < cellWidth / 2, abs(p.y - c.y) < rowHeight / 2 {
                    return index
                }
            }
        }
        for index in 0 ..< colorCellCount {
            let c = colorCellCenter(index)
            if abs(p.x - c.x) < cellWidth / 2, abs(p.y - c.y) < rowHeight / 2 {
                return 100 + index
            }
        }
        return nil
    }

    // MARK: - Interaction

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        trackingAreas.forEach { removeTrackingArea($0) }
        addTrackingArea(NSTrackingArea(
            rect: bounds,
            options: [.mouseMoved, .mouseEnteredAndExited, .activeAlways],
            owner: self,
        ))
    }

    override func mouseMoved(with event: NSEvent) {
        // We're a custom-drawn control surface, not crosshair country —
        // without this the selection view's cursor sticks.
        NSCursor.arrow.set()
        let p = convert(event.locationInWindow, from: nil)
        let hit = hitCell(at: p)
        if hit != hoverIndex {
            hoverIndex = hit
            needsDisplay = true
        }
    }

    override func mouseExited(with _: NSEvent) {
        hoverIndex = nil
        needsDisplay = true
    }

    /// Handle the click on mouse-DOWN (snappier, and swallows the
    /// event). Without an override here the press would propagate up
    /// to the SelectionView and be read as "click outside → new
    /// selection", wiping the annotations the user is styling.
    override func mouseDown(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        guard let hit = hitCell(at: p) else { return }

        if hit < 100 {
            // Variant row.
            mutateStyle { style in
                switch activeTool {
                case .text: style.fontSize = Self.fontSizes[hit]
                case .mosaic: style.mosaicLevel = hit
                default: style.lineWidth = Self.widths[hit]
                }
            }
        } else {
            let colorIndex = hit - 100
            if colorIndex == Self.colors.count {
                openColorPanel()
                return
            }
            mutateStyle { $0.color = Self.colors[colorIndex] }
        }
        onChange?()
        needsDisplay = true
    }

    override func mouseUp(with _: NSEvent) {
        // Swallowed — see mouseDown.
    }

    // MARK: - Custom color

    private func openColorPanel() {
        let panel = NSColorPanel.shared
        panel.color = customColor ?? style.color
        panel.isContinuous = true
        panel.showsAlpha = false
        panel.setTarget(self)
        panel.setAction(#selector(colorPanelChanged(_:)))
        panel.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    @objc private func colorPanelChanged(_ panel: NSColorPanel) {
        customColor = panel.color
        mutateStyle { $0.color = panel.color }
        onChange?()
        needsDisplay = true
    }

    // MARK: - Drawing

    override func draw(_: NSRect) {
        if hasVariantRow {
            drawVariantRow()
        }
        drawColorRow()
    }

    private func drawVariantRow() {
        guard let activeTool else { return }
        for index in 0 ..< 3 {
            let center = variantCellCenter(index)
            let selected = isVariantSelected(index)
            if selected {
                drawSelectionRing(at: center, radius: 13)
            } else if hoverIndex == index {
                drawHoverRing(at: center, radius: 13)
            }
            switch activeTool {
            case .text:
                let size = Self.fontSizes[index]
                let attributes: [NSAttributedString.Key: Any] = [
                    .font: NSFont.systemFont(ofSize: size, weight: .semibold),
                    .foregroundColor: style.color,
                ]
                let glyph = "A"
                let glyphSize = glyph.size(withAttributes: attributes)
                glyph.draw(
                    at: CGPoint(x: center.x - glyphSize.width / 2,
                                y: center.y - glyphSize.height / 2),
                    withAttributes: attributes,
                )
            case .mosaic:
                drawMosaicPreview(at: center, level: index)
            default:
                // Stroke width: a real line segment in the current color.
                let line = NSBezierPath()
                line.lineWidth = Self.widths[index]
                line.lineCapStyle = .round
                line.move(to: CGPoint(x: center.x - 8, y: center.y))
                line.line(to: CGPoint(x: center.x + 8, y: center.y))
                style.color.setStroke()
                line.stroke()
            }
        }
    }

    private func isVariantSelected(_ index: Int) -> Bool {
        switch activeTool {
        case .text: return Self.fontSizes[index] == style.fontSize
        case .mosaic: return index == style.mosaicLevel
        default: return Self.widths[index] == style.lineWidth
        }
    }

    private func drawMosaicPreview(at center: CGPoint, level: Int) {
        // 3×3 checkerboard whose blocks grow with the level.
        let block: CGFloat = [3, 4.5, 6][level]
        let origin = CGPoint(x: center.x - block * 1.5, y: center.y - block * 1.5)
        for row in 0 ..< 3 {
            for col in 0 ..< 3 where (row + col) % 2 == 0 {
                let rect = CGRect(
                    x: origin.x + CGFloat(col) * block,
                    y: origin.y + CGFloat(row) * block,
                    width: block, height: block,
                )
                NSColor(white: 0.35, alpha: 1).setFill()
                NSBezierPath(rect: rect).fill()
            }
        }
    }

    private func drawColorRow() {
        for (index, color) in Self.colors.enumerated() {
            drawSwatch(color, at: colorCellCenter(index), selected: color == style.color, index: 100 + index)
        }
        // Custom color "+" cell: shows the picked color once set.
        let center = colorCellCenter(Self.colors.count)
        if let customColor {
            drawSwatch(customColor, at: center, selected: customColor == style.color, index: 100 + Self.colors.count)
        } else {
            let ring = NSBezierPath(ovalIn: CGRect(
                x: center.x - 8, y: center.y - 8, width: 16, height: 16,
            ))
            NSColor(white: 0.45, alpha: 1).setStroke()
            ring.lineWidth = 1
            ring.stroke()
            let plus = NSBezierPath()
            plus.lineWidth = 1.4
            plus.move(to: CGPoint(x: center.x - 4, y: center.y))
            plus.line(to: CGPoint(x: center.x + 4, y: center.y))
            plus.move(to: CGPoint(x: center.x, y: center.y - 4))
            plus.line(to: CGPoint(x: center.x, y: center.y + 4))
            plus.stroke()
            if hoverIndex == 100 + Self.colors.count {
                drawHoverRing(at: center, radius: 12)
            }
        }
    }

    private func drawSwatch(_ color: NSColor, at center: CGPoint, selected: Bool, index: Int) {
        let rect = CGRect(x: center.x - 8, y: center.y - 8, width: 16, height: 16)
        let dot = NSBezierPath(ovalIn: rect)
        color.setFill()
        dot.fill()
        // Border so white/light swatches stay visible on the white pill.
        NSColor.black.withAlphaComponent(0.2).setStroke()
        dot.lineWidth = 0.5
        dot.stroke()
        if selected {
            drawSelectionRing(at: center, radius: 12)
        } else if hoverIndex == index {
            drawHoverRing(at: center, radius: 12)
        }
    }

    private func drawSelectionRing(at center: CGPoint, radius: CGFloat) {
        let ring = NSBezierPath(ovalIn: CGRect(
            x: center.x - radius, y: center.y - radius,
            width: radius * 2, height: radius * 2,
        ))
        Self.accentBlue.setStroke()
        ring.lineWidth = 2
        ring.stroke()
    }

    private func drawHoverRing(at center: CGPoint, radius: CGFloat) {
        let ring = NSBezierPath(ovalIn: CGRect(
            x: center.x - radius, y: center.y - radius,
            width: radius * 2, height: radius * 2,
        ))
        NSColor.black.withAlphaComponent(0.15).setStroke()
        ring.lineWidth = 1
        ring.stroke()
    }
}
