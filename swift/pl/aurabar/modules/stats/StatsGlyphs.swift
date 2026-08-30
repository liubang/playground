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
        return makeLabeled(caption: "CPU", value: value, valueWidthReference: "100%") { rect in
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
        // Formatters.bytes caps out at "xx.xU" — "99.9G" is the widest
        // shape (the dot is narrower than a digit).
        makeLabeled(caption: "MEM", value: value, valueWidthReference: "99.9G") { rect in
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

    // MARK: - Battery

    /// Battery outline + nub + fill level + "BAT" caption + percentage.
    /// Charging draws a bolt: punched through the fill when there's
    /// enough of it, solid on an empty battery.
    static func makeBattery(fraction: Double, charging: Bool, value: String) -> NSImage {
        let fraction = min(max(fraction, 0), 1)
        return makeLabeled(caption: "BAT", value: value, valueWidthReference: "100%") { rect in
            let body = rect.insetBy(dx: 1.2, dy: 4.2)
            let outline = NSBezierPath(roundedRect: body, xRadius: 2.2, yRadius: 2.2)
            outline.lineWidth = 1.4
            NSColor.black.setStroke()
            outline.stroke()

            // Nub on the right edge.
            let nub = NSRect(x: body.maxX + 0.6, y: body.midY - 1.8, width: 1.4, height: 3.6)
            NSColor.black.setFill()
            NSBezierPath(roundedRect: nub, xRadius: 0.7, yRadius: 0.7).fill()

            // Fill level.
            let inner = body.insetBy(dx: 1.4, dy: 1.4)
            let fillWidth = inner.width * fraction
            if fillWidth > 0.4 {
                NSBezierPath(
                    roundedRect: NSRect(x: inner.minX, y: inner.minY, width: fillWidth, height: inner.height),
                    xRadius: 1,
                    yRadius: 1,
                ).fill()
            }

            if charging {
                let cx = body.midX
                let cy = body.midY
                let bolt = NSBezierPath()
                bolt.move(to: NSPoint(x: cx + 1.4, y: cy + 3.4))
                bolt.line(to: NSPoint(x: cx - 1.8, y: cy + 0.3))
                bolt.line(to: NSPoint(x: cx - 0.1, y: cy + 0.3))
                bolt.line(to: NSPoint(x: cx - 1.4, y: cy - 3.4))
                bolt.line(to: NSPoint(x: cx + 1.8, y: cy - 0.3))
                bolt.line(to: NSPoint(x: cx + 0.1, y: cy - 0.3))
                bolt.close()
                if let context = NSGraphicsContext.current {
                    let cg = context.cgContext
                    cg.saveGState()
                    // Punch the bolt through as a hole when the fill
                    // covers it; draw it solid otherwise.
                    cg.setBlendMode(fraction > 0.25 ? .clear : .normal)
                    bolt.fill()
                    cg.restoreGState()
                }
            }
        }
    }

    // MARK: - GPU

    /// Fan glyph + "GPU" caption + percentage. The blades' opacity
    /// follows utilization, so an idle fan reads faint and a busy one
    /// solid — a live reading even before the eye reaches the value.
    static func makeGPU(fraction: Double, value: String) -> NSImage {
        let fraction = min(max(fraction, 0), 1)
        return makeLabeled(caption: "GPU", value: value, valueWidthReference: "100%") { rect in
            let center = NSPoint(x: rect.midX, y: rect.midY)
            let radius = min(rect.width, rect.height) / 2 - 1.1

            NSColor.black.withAlphaComponent(0.3 + 0.7 * fraction).setFill()
            // Three blades: 95° wedges around the hub with 25° gaps.
            for blade in 0 ..< 3 {
                let start = CGFloat(blade) * 120 + 12
                let path = NSBezierPath()
                path.appendArc(
                    withCenter: center,
                    radius: radius,
                    startAngle: start,
                    endAngle: start + 95,
                )
                path.appendArc(
                    withCenter: center,
                    radius: 2.6,
                    startAngle: start + 95,
                    endAngle: start,
                    clockwise: true,
                )
                path.close()
                path.fill()
            }

            // Hub.
            NSColor.black.setFill()
            NSBezierPath(ovalIn: NSRect(
                x: center.x - 1.7,
                y: center.y - 1.7,
                width: 3.4,
                height: 3.4,
            )).fill()
        }
    }

    // MARK: - Network

    /// Two value lines (up over down) — no leading glyph, the ↑/↓ in the
    /// text already say it. A direction with no traffic right now (<1K/s)
    /// is dimmed.
    static func makeNetwork(up: Double, down: Double) -> NSImage {
        // Pad rates to a fixed width with figure spaces so the item
        // doesn't jitter its neighbors as numbers change length.
        let upText = "↑\(padded(Formatters.rate(up)))"
        let downText = "↓\(padded(Formatters.rate(down)))"
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

    // MARK: - Disk

    /// Drive icon + two value lines (write over read). Deliberately
    /// not the network glyph's ↑/↓ arrows: with both modules visible,
    /// two arrow pairs read as duplicates, and R/W is the disk
    /// language (Activity Monitor) rather than a borrowed network
    /// metaphor. Direction letters render dim like the captions;
    /// a direction idling below 1K/s dims its value too.
    static func makeDisk(read: Double, write: Double) -> NSImage {
        let writeText = padded(Formatters.rate(write))
        let readText = padded(Formatters.rate(read))
        let letterAttrs = valueAttributes(size: 8.5, dimmed: true)
        let writeAttrs = valueAttributes(size: 8.5, dimmed: write < 1024)
        let readAttrs = valueAttributes(size: 8.5, dimmed: read < 1024)
        let letterWidth = textWidth("W ", letterAttrs)
        let textWidthMax = max(textWidth(writeText, writeAttrs), textWidth(readText, readAttrs))
        let width = iconBox + gap + letterWidth + textWidthMax

        let image = NSImage(size: NSSize(width: width, height: height), flipped: false) { _ in
            drawDrive(NSRect(x: 0, y: (height - iconBox) / 2, width: iconBox, height: iconBox))
            let letterX = iconBox + gap
            let valueX = letterX + letterWidth
            drawFlipped("W ", topLeft: NSPoint(x: letterX, y: 9.4), attributes: letterAttrs)
            drawFlipped(writeText, topLeft: NSPoint(x: valueX, y: 9.4), attributes: writeAttrs)
            drawFlipped("R ", topLeft: NSPoint(x: letterX, y: 0.4), attributes: letterAttrs)
            drawFlipped(readText, topLeft: NSPoint(x: valueX, y: 0.4), attributes: readAttrs)
            return true
        }
        image.isTemplate = true
        return image
    }

    /// Internal-drive silhouette: horizontal rounded rectangle with a
    /// slot line near the bottom edge.
    private static func drawDrive(_ rect: NSRect) {
        let body = rect.insetBy(dx: 1.4, dy: 3.4)
        let outline = NSBezierPath(roundedRect: body, xRadius: 2.4, yRadius: 2.4)
        outline.lineWidth = 1.4
        NSColor.black.setStroke()
        outline.stroke()

        NSColor.black.setFill()
        NSBezierPath(
            roundedRect: NSRect(
                x: body.minX + 2,
                y: body.minY + 1.3,
                width: body.width - 4,
                height: 1.1,
            ),
            xRadius: 0.55,
            yRadius: 0.55,
        ).fill()
    }

    // MARK: - Composite layout

    /// Icon on the left, caption over value on the right. The image
    /// width follows `valueWidthReference` (the widest value the module
    /// can show), not the current value: with monospaced digits the
    /// value's width changes with its length, and hugging it would make
    /// the status item — and everything to its left — jump every time a
    /// reading crosses 9%→10%. Text is left-aligned as before.
    private static func makeLabeled(
        caption: String,
        value: String,
        valueWidthReference: String,
        drawIcon: @escaping (NSRect) -> Void,
    ) -> NSImage {
        let valueAttrs = valueAttributes(size: 9.5, dimmed: false)
        let width = iconBox + gap + textWidth(valueWidthReference, valueAttrs)
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

    /// Left-pads to 4 characters with figure spaces (digit-width, so the
    /// label stays visually monospaced).
    private static func padded(_ text: String) -> String {
        let missing = 4 - text.count
        return missing > 0 ? String(repeating: "\u{2007}", count: missing) + text : text
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
