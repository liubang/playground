import AppKit

/// Hand-drawn menu bar icons for the stats items, following Stats'
/// widget style: a small live gauge on the left and a two-line label on
/// the right (tiny module caption over the value), composited into one
/// template image so it adapts to the menu bar's light/dark appearance.
/// Redrawn every sample (2s) — trivial cost at this size.
///
/// Value strings never contain descenders (digits, %, K/M/G/T, arrows),
/// so the two text lines can be packed tightly without clipping.
///
/// Glyph sizing: every icon is drawn to a common optical envelope, so
/// even though the modules occupy identical boxes, no single glyph reads
/// bigger or smaller than its neighbors — round glyphs (CPU donut, GPU
/// fan) ink at 14pt diameter, wide glyphs (battery, drive) at ~14×10.5,
/// the memory vessel and the calendar sheet at ~12.5×13.5. Every icon's
/// left ink edge sits exactly 2pt from its image edge so the perceived
/// inter-module spacing is even. makeSymbol normalizes SF Symbols to a
/// fixed ink height so the weather module joins the family.
enum StatsGlyphs {
    private static let height: CGFloat = 18
    private static let iconBox: CGFloat = 16
    private static let gap: CGFloat = 3
    /// Uniform empty margin on both sides of every glyph image. AppKit
    /// spaces status items by a fixed amount, so the *perceived* gap
    /// between two modules is our edge margins on both neighbors plus
    /// that constant — identical margins across modules are what makes
    /// the bar read as evenly spaced. Kept at 1: evenness only needs
    /// *equal* margins, and every point counts on notched MacBooks
    /// where menu bar real estate overflows into the notch. MenuBarGlyph
    /// shares the constant.
    static let edgeMargin: CGFloat = 1

    // MARK: - CPU

    /// Donut gauge + "CPU" caption + percentage value.
    static func makeCPU(fraction: Double, value: String) -> NSImage {
        let fraction = min(max(fraction, 0), 1)
        return makeLabeled(caption: "CPU", value: value, valueWidthReference: "100%") { rect in
            // Inset 2.1 + half the 2.2pt stroke puts the left ink edge
            // at exactly 2pt, matching the other glyphs.
            let inset = rect.insetBy(dx: 2.1, dy: 2.1)
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
        // "99.9G" is the widest bytes shape: between 10 and 99.9 of a
        // unit the formatter keeps one decimal, and the dot makes that
        // ~3pt wider than the 3-digit integer shape ("888G"). At 100+
        // the decimal is dropped ("128G"); 1TB+ RAM reads "1.5T" —
        // both shorter.
        makeLabeled(caption: "MEM", value: value, valueWidthReference: "99.9G") { rect in
            // Inset 1.75 + half the 1.5pt stroke puts the left ink edge
            // at exactly 2pt, matching the other glyphs.
            let inset = rect.insetBy(dx: 1.75, dy: 1.8)
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
            let body = rect.insetBy(dx: 1.8, dy: 3.5)
            let outline = NSBezierPath(roundedRect: body, xRadius: 2.2, yRadius: 2.2)
            outline.lineWidth = 1.4
            NSColor.black.setStroke()
            outline.stroke()

            // Nub on the right edge, kept inside the icon box.
            let nub = NSRect(x: body.maxX + 0.5, y: body.midY - 1.9, width: 1.3, height: 3.8)
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
            // Radius 7: 14pt ink diameter, left ink edge at 2pt.
            let radius = min(rect.width, rect.height) / 2 - 1.0

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

            // Hub, sized to meet the blade roots (inner radius 2.6) so
            // the fan reads as one mass instead of dot + ring + blades.
            NSColor.black.setFill()
            NSBezierPath(ovalIn: NSRect(
                x: center.x - 2.6,
                y: center.y - 2.6,
                width: 5.2,
                height: 5.2,
            )).fill()
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
        // Fixed width from the widest possible text. That is "↑99.9M",
        // not "↑888M": rates between 10 and 99.9 of a unit keep one
        // decimal ("36.2M"), and the dot makes that shape ~3pt wider
        // than the integer one — "↑888M" let the ink touch the image's
        // right edge. Lines are centered in the reserved column so the
        // slack splits evenly (same rule makeLabeled applies); hugging
        // the current text would shift the neighbors as rates cross
        // 10 or 100 of a unit. Baselines match makeLabeled's rows.
        let width = edgeMargin + textWidth("↑99.9M", upAttrs) + edgeMargin

        let image = NSImage(size: NSSize(width: width, height: height), flipped: false) { _ in
            let upX = (width - textWidth(upText, upAttrs)) / 2
            let downX = (width - textWidth(downText, downAttrs)) / 2
            drawFlipped(upText, topLeft: NSPoint(x: upX, y: 9.6), attributes: upAttrs)
            drawFlipped(downText, topLeft: NSPoint(x: downX, y: 0.2), attributes: downAttrs)
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
        let writeText = Formatters.rate(write)
        let readText = Formatters.rate(read)
        let letterAttrs = valueAttributes(size: 8.5, dimmed: true)
        let writeAttrs = valueAttributes(size: 8.5, dimmed: write < 1024)
        let readAttrs = valueAttributes(size: 8.5, dimmed: read < 1024)
        let letterWidth = textWidth("W ", letterAttrs)
        // Fixed value column from the widest rate shape — "99.9M", not
        // "888M": rates between 10 and 99.9 of a unit keep one decimal
        // ("42.9M"), which is wider than the integer shape (same fix as
        // the network glyph). Values are right-aligned in the reserved
        // column, so the module's right ink edge never moves.
        let valueColumnWidth = textWidth("99.9M", writeAttrs)
        let width = edgeMargin + iconBox + gap + letterWidth + valueColumnWidth + edgeMargin

        let image = NSImage(size: NSSize(width: width, height: height), flipped: false) { _ in
            drawDrive(NSRect(
                x: edgeMargin,
                y: (height - iconBox) / 2,
                width: iconBox,
                height: iconBox,
            ))
            let letterX = edgeMargin + iconBox + gap
            let valueRight = width - edgeMargin
            let writeX = valueRight - textWidth(writeText, writeAttrs)
            let readX = valueRight - textWidth(readText, readAttrs)
            drawFlipped("W ", topLeft: NSPoint(x: letterX, y: 9.6), attributes: letterAttrs)
            drawFlipped(writeText, topLeft: NSPoint(x: writeX, y: 9.6), attributes: writeAttrs)
            drawFlipped("R ", topLeft: NSPoint(x: letterX, y: 0.2), attributes: letterAttrs)
            drawFlipped(readText, topLeft: NSPoint(x: readX, y: 0.2), attributes: readAttrs)
            return true
        }
        image.isTemplate = true
        return image
    }

    /// Internal-drive silhouette: horizontal rounded rectangle with a
    /// slot line near the bottom edge.
    private static func drawDrive(_ rect: NSRect) {
        let body = rect.insetBy(dx: 1.8, dy: 3.3)
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

    // MARK: - SF Symbol (shared box)

    /// An SF Symbol normalized into the same optical envelope the
    /// hand-drawn glyphs use. Raw `NSImage(systemSymbolName:)` sizes
    /// each symbol to its own natural metrics — sun.max.fill inks at
    /// ~13.4pt tall at 13pt while cloud.fill inks at ~9.3 — so the
    /// weather icon used to change visual weight with the condition.
    /// Here the symbol's ink bounding box is measured once and scaled
    /// to a fixed ink height (capped by width), centered by its ink.
    static func makeSymbol(_ name: String, pointSize: CGFloat = 13) -> NSImage {
        let config = NSImage.SymbolConfiguration(pointSize: pointSize, weight: .medium)
        guard let symbol = NSImage(systemSymbolName: name, accessibilityDescription: nil)?
            .withSymbolConfiguration(config)
        else {
            return NSImage()
        }
        let ink = inkBounds(of: symbol) ?? NSRect(origin: .zero, size: symbol.size)
        let targetInkHeight: CGFloat = 12.5
        let maxInkWidth: CGFloat = 15.5
        let scale = min(targetInkHeight / ink.height, maxInkWidth / ink.width)
        let width = edgeMargin + iconBox + edgeMargin
        let image = NSImage(size: NSSize(width: width, height: height), flipped: false) { _ in
            symbol.draw(in: NSRect(
                x: width / 2 - ink.midX * scale,
                y: height / 2 - ink.midY * scale,
                width: symbol.size.width * scale,
                height: symbol.size.height * scale,
            ))
            return true
        }
        image.isTemplate = true
        return image
    }

    /// The pixel bounding box of a symbol's ink, in the image's own
    /// (y-up) coordinate space. Measured by rasterizing at 2x — done
    /// once per weather-condition change, so the cost is irrelevant.
    private static func inkBounds(of image: NSImage) -> NSRect? {
        let scale: CGFloat = 2
        let w = Int((image.size.width * scale).rounded(.up))
        let h = Int((image.size.height * scale).rounded(.up))
        guard w > 0, h > 0, let rep = NSBitmapImageRep(
            bitmapDataPlanes: nil, pixelsWide: w, pixelsHigh: h,
            bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
            colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0,
        ) else { return nil }
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
        image.draw(in: NSRect(x: 0, y: 0, width: CGFloat(w), height: CGFloat(h)))
        NSGraphicsContext.restoreGraphicsState()
        guard let data = rep.bitmapData else { return nil }
        let bpp = rep.bitsPerPixel / 8
        var minX = w, minY = h, maxX = -1, maxY = -1
        for row in 0 ..< h {
            for col in 0 ..< w {
                if data[row * rep.bytesPerRow + col * bpp + 3] > 16 {
                    minX = min(minX, col)
                    maxX = max(maxX, col)
                    minY = min(minY, row)
                    maxY = max(maxY, row)
                }
            }
        }
        guard maxX >= minX, maxY >= minY else { return nil }
        // Bitmap row 0 is the image's top edge; convert to y-up points.
        return NSRect(
            x: CGFloat(minX) / scale,
            y: CGFloat(h - maxY - 1) / scale,
            width: CGFloat(maxX - minX + 1) / scale,
            height: CGFloat(maxY - minY + 1) / scale,
        )
    }

    // MARK: - Composite layout

    /// Icon on the left, caption over value on the right. The image
    /// width follows `valueWidthReference` (the widest value the module
    /// can show), not the current value: with monospaced digits the
    /// value's width changes with its length, and hugging it would make
    /// the status item — and everything to its left — jump every time a
    /// reading crosses 9%→10%. Both text lines are centered in that
    /// reserved column: left-aligning a short value ("7%" in a "100%"
    /// column) parks all the slack on the right, where it reads as a
    /// wider gap to the next module; centering splits it evenly.
    private static func makeLabeled(
        caption: String,
        value: String,
        valueWidthReference: String,
        drawIcon: @escaping (NSRect) -> Void,
    ) -> NSImage {
        let valueAttrs = valueAttributes(size: 9.5, dimmed: false)
        let columnWidth = textWidth(valueWidthReference, valueAttrs)
        let width = edgeMargin + iconBox + gap + columnWidth + edgeMargin
        let image = NSImage(size: NSSize(width: width, height: height), flipped: false) { _ in
            let iconRect = NSRect(
                x: edgeMargin,
                y: (height - iconBox) / 2,
                width: iconBox,
                height: iconBox,
            )
            drawIcon(iconRect)

            let textX = edgeMargin + iconBox + gap
            // Clamped at 0: a value wider than the reference (shouldn't
            // happen) stays left-aligned instead of clipping the icon.
            let captionX = textX + max(0, (columnWidth - textWidth(caption, captionAttributes)) / 2)
            let valueX = textX + max(0, (columnWidth - textWidth(value, valueAttrs)) / 2)
            drawFlipped(caption, topLeft: NSPoint(x: captionX, y: 10.2), attributes: captionAttributes)
            drawFlipped(value, topLeft: NSPoint(x: valueX, y: 0.2), attributes: valueAttrs)
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
