import AppKit

/// The small calendar glyph shown next to the clock text in the menu bar:
/// a rounded calendar sheet with a header band and a 3×2 dot grid, one
/// dot filled to suggest "today". Drawn as a template image so it adapts
/// to the menu bar's light/dark appearance automatically.
enum MenuBarGlyph {
    static func make() -> NSImage {
        let side: CGFloat = 18
        let image = NSImage(size: NSSize(width: side, height: side), flipped: false) { rect in
            let inset = rect.insetBy(dx: 2.2, dy: 2.2)
            let sheet = NSBezierPath(roundedRect: inset, xRadius: 3.4, yRadius: 3.4)
            sheet.lineWidth = 1.5
            NSColor.black.setStroke()
            sheet.stroke()

            // Header band.
            let bandY = inset.maxY - 3.6
            let band = NSBezierPath()
            band.move(to: NSPoint(x: inset.minX + 1.2, y: bandY))
            band.line(to: NSPoint(x: inset.maxX - 1.2, y: bandY))
            band.lineWidth = 1.2
            band.stroke()

            // 3×2 dot grid below the band; the center dot is filled.
            let cols: CGFloat = 3
            let rows: CGFloat = 2
            let gridWidth = inset.width - 5.6
            let gridHeight = bandY - inset.minY - 3.4
            let cellW = gridWidth / cols
            let cellH = gridHeight / rows
            for row in 0 ..< Int(rows) {
                for col in 0 ..< Int(cols) {
                    let cx = inset.minX + 2.8 + cellW * (CGFloat(col) + 0.5)
                    let cy = bandY - 1.6 - cellH * (CGFloat(row) + 0.5)
                    let dotRect = NSRect(x: cx - 1.05, y: cy - 1.05, width: 2.1, height: 2.1)
                    let dot = NSBezierPath(ovalIn: dotRect)
                    if row == 0, col == 1 {
                        NSColor.black.setFill()
                        dot.fill()
                    } else {
                        NSColor.black.withAlphaComponent(0.45).setFill()
                        dot.fill()
                    }
                }
            }
            return true
        }
        image.isTemplate = true
        return image
    }
}
