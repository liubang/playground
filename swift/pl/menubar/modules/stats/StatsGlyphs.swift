import AppKit

/// Hand-drawn menu bar icons for the stats items, following Stats'
/// widget style: a small live gauge on the left and a two-line label on
/// the right (tiny module caption over the value), composited into one
/// template image so it adapts to the menu bar's light/dark appearance.
/// Redrawn every sample (2s) — trivial cost at this size.
///
/// Value strings never contain descenders (digits, %, K/M/G/T, arrows),
/// so the two text lines can be packed tightly without clipping.
enum StatsGlyphs {
    private static let height: CGFloat = 18
    private static let iconBox: CGFloat = 16
    private static let gap: CGFloat = 3

    // MARK: - CPU

    /// Donut gauge + "CPU" caption + percentage value.
    static func makeCPU(fraction: Double, value: String) -> NSImage {
        let fraction = min(max(fraction, 0), 1)
        return makeLabeled(caption: "CPU", value: value) { rect in
            let inset = rect.insetBy(dx: 1.7, dy: 1.7)
            let center = NSPoint(x: inset.midX, y: inset.midY)
            let radius = inset.width / 2

            let track = NSBezierPath(ovalIn: inset)
            track.lineWidth = 2.2
            NSColor.black.withAlphaComponent(0.28).setStroke()
            track.stroke()

            guard fraction > 0.005 else { return }
            let arc = NSBezierPath()
            arc.appendArc(
                withCenter: center,
                radius: radius,
                startAngle: 90,
                endAngle: 90 - 360 * fraction,
                clockwise: true,
            )
            arc.lineWidth = 2.2
            arc.lineCapStyle = .round
            NSColor.black.setStroke()
            arc.stroke()
        }
    }

    // MARK: - Memory

    /// Level vessel + "MEM" caption + used-bytes value.
    static func makeMemory(fraction: Double, value: String) -> NSImage {
        let fraction = min(max(fraction, 0), 1)
        return makeLabeled(caption: "MEM", value: value) { rect in
            let inset = rect.insetBy(dx: 3.0, dy: 1.4)
            let vessel = NSBezierPath(roundedRect: inset, xRadius: 2.4, yRadius: 2.4)
            vessel.lineWidth = 1.5
            NSColor.black.setStroke()
            vessel.stroke()

            let inner = inset.insetBy(dx: 1.5, dy: 1.5)
            let fillHeight = inner.height * fraction
            guard fillHeight > 0.4 else { return }
            NSGraphicsContext.saveGraphicsState()
            NSBezierPath(roundedRect: inner, xRadius: 1.4, yRadius: 1.4).addClip()
            NSColor.black.setFill()
            NSRect(
                x: inner.minX,
                y: inner.minY,
                width: inner.width,
                height: fillHeight,
            ).fill()
            NSGraphicsContext.restoreGraphicsState()
        }
    }

    // MARK: - Network

    /// Two value lines (up over down) — no leading glyph, the ↑/↓ in the
    /// text already say it. A direction with no traffic right now (<1K/s)
    /// is dimmed.
    static func makeNetwork(up: Double, down: Double) -> NSImage {
        let upText = "↑\(Formatters.rate(up))"
        let downText = "↓\(Formatters.rate(down))"
        let upAttrs = valueAttributes(size: 8.5, dimmed: up < 1024)
        let downAttrs = valueAttributes(size: 8.5, dimmed: down < 1024)
        let width = max(textWidth(upText, upAttrs), textWidth(downText, downAttrs))

        let image = NSImage(size: NSSize(width: width, height: height), flipped: false) { _ in
            drawFlipped(upText, topLeft: NSPoint(x: 0, y: 9.4), attributes: upAttrs)
            drawFlipped(downText, topLeft: NSPoint(x: 0, y: 0.4), attributes: downAttrs)
            return true
        }
        image.isTemplate = true
        return image
    }

    // MARK: - Composite layout

    /// Icon on the left, caption over value on the right. The image
    /// width hugs the value text so the item stays compact.
    private static func makeLabeled(
        caption: String,
        value: String,
        drawIcon: @escaping (NSRect) -> Void,
    ) -> NSImage {
        let valueAttrs = valueAttributes(size: 9.5, dimmed: false)
        let width = iconBox + gap + textWidth(value, valueAttrs)
        let image = NSImage(size: NSSize(width: width, height: height), flipped: false) { _ in
            let iconRect = NSRect(x: 0, y: (height - iconBox) / 2, width: iconBox, height: iconBox)
            drawIcon(iconRect)

            let textX = iconBox + gap
            drawFlipped(caption, topLeft: NSPoint(x: textX, y: 10.2), attributes: captionAttributes)
            drawFlipped(value, topLeft: NSPoint(x: textX, y: 0.2), attributes: valueAttrs)
            return true
        }
        image.isTemplate = true
        return image
    }

    // MARK: - Drawing helpers

    private static let captionAttributes: [NSAttributedString.Key: Any] = [
        .font: NSFont.systemFont(ofSize: 6, weight: .bold),
        .foregroundColor: NSColor.black.withAlphaComponent(0.6),
        .kern: 0.6,
    ]

    private static func valueAttributes(size: CGFloat, dimmed: Bool) -> [NSAttributedString.Key: Any] {
        [
            .font: NSFont.monospacedDigitSystemFont(ofSize: size, weight: .medium),
            .foregroundColor: NSColor.black.withAlphaComponent(dimmed ? 0.35 : 1),
        ]
    }

    private static func textWidth(_ text: String, _ attributes: [NSAttributedString.Key: Any]) -> CGFloat {
        ceil((text as NSString).size(withAttributes: attributes).width)
    }

    /// Draws a single text line. AppKit's NSString drawing auto-adjusts
    /// for the context's flipped state (and ignores manual CTM flips),
    /// so no transform is applied here. In our y-up image the given
    /// point is the line's bottom-left origin — text extends upward.
    private static func drawFlipped(
        _ text: String,
        topLeft: NSPoint,
        attributes: [NSAttributedString.Key: Any],
    ) {
        (text as NSString).draw(at: topLeft, withAttributes: attributes)
    }
}
