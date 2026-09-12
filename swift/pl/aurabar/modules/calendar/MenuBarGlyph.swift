import AppKit

/// The small calendar glyph shown next to the clock text in the menu bar:
/// a rounded calendar sheet with a header band and a 3×2 dot grid, one
/// dot filled to suggest "today". Drawn as a template image so it adapts
/// to the menu bar's light/dark appearance automatically.
///
/// Sized to the stats modules' optical envelope: an 18pt-tall image with
/// ~12.5×13.5pt of ink whose left edge sits 2pt in — the same numbers
/// the CPU donut, GPU fan and memory vessel use, so no module reads
/// bigger than its neighbors. (The previous 20pt image with 17pt of ink
/// made the calendar the visual giant of the row.)
enum MenuBarGlyph {
    static func make() -> NSImage {
        // Same uniform edge margin the stats glyphs use, so the
        // perceived gap to the neighboring status items is identical.
        let margin = StatsGlyphs.edgeMargin
        let image = NSImage(
            size: NSSize(width: margin + 16 + margin, height: 18),
            flipped: false,
        ) { rect in
            // The 1.5pt stroke is centered on the path, so the ink
            // extends 0.75pt past these insets: left ink at 2pt.
            let inset = rect.insetBy(dx: margin + 1.75, dy: 2.3)
            let sheet = NSBezierPath(roundedRect: inset, xRadius: 3, yRadius: 3)
            sheet.lineWidth = 1.5
            NSColor.black.setStroke()
            sheet.stroke()

            // Header band.
            let bandY = inset.maxY - 3.3
            let band = NSBezierPath()
            band.move(to: NSPoint(x: inset.minX + 1.1, y: bandY))
            band.line(to: NSPoint(x: inset.maxX - 1.1, y: bandY))
            band.lineWidth = 1.2
            band.stroke()

            // 3×2 dot grid below the band; the center dot is filled.
            let cols: CGFloat = 3
            let rows: CGFloat = 2
            let gridWidth = inset.width - 5
            let gridHeight = bandY - inset.minY - 3.1
            let cellW = gridWidth / cols
            let cellH = gridHeight / rows
            for row in 0 ..< Int(rows) {
                for col in 0 ..< Int(cols) {
                    let cx = inset.minX + 2.5 + cellW * (CGFloat(col) + 0.5)
                    let cy = bandY - 1.5 - cellH * (CGFloat(row) + 0.5)
                    let dotRect = NSRect(x: cx - 1, y: cy - 1, width: 2, height: 2)
                    let dot = NSBezierPath(ovalIn: dotRect)
                    if row == 0, col == 1 {
                        NSColor.black.setFill()
                    } else {
                        NSColor.black.withAlphaComponent(0.45).setFill()
                    }
                    dot.fill()
                }
            }
            return true
        }
        image.isTemplate = true
        return image
    }
}
