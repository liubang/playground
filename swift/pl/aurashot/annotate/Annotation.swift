import AppKit

/// The annotation tools offered by the toolbar: shapes, freehand,
/// pixelate, text, numbered step markers.
enum AnnotationTool: String, CaseIterable {
    case rectangle
    case ellipse
    case line
    case arrow
    case freehand
    case mosaic
    case text
    case step
}

/// One annotation as pure value state: the state model is kept
/// separate from rendering. All coordinates are view-local points
/// (bottom-left origin), identical in the overlay and at export time
/// after the compositor's transform.
///
/// Rendering draws into the CURRENT NSGraphicsContext, so the same
/// code path serves both the on-screen overlay and offscreen export
/// (ImageCompositor sets up a bitmap context with the right transform).
struct Annotation {
    var tool: AnnotationTool
    var start: CGPoint
    var end: CGPoint
    var text: String = ""
    var color: NSColor = .systemRed
    var lineWidth: CGFloat = 2.5
    var fontSize: CGFloat = 16
    /// PostScript name of the font chosen in the palette; nil keeps
    /// the historical default (bold system font).
    var fontName: String?

    /// Freehand only: the stroke's sample points.
    var points: [CGPoint] = []
    /// Step only: the 1-based marker number.
    var number: Int = 1
    /// Mosaic only: the palette intensity the patch was baked with, so
    /// re-bakes after a move/resize keep THIS region's look even if the
    /// palette level has since changed.
    var mosaicScale: CGFloat = 1.0
    /// Mosaic only: the pre-rendered pixellated patch, captured at
    /// commit time at pixel resolution (sharp on Retina exports).
    var patch: NSImage?

    var rect: CGRect {
        switch tool {
        case .freehand:
            guard let first = points.first else { return .zero }
            var minX = first.x, minY = first.y, maxX = first.x, maxY = first.y
            for p in points.dropFirst() {
                minX = min(minX, p.x); maxX = max(maxX, p.x)
                minY = min(minY, p.y); maxY = max(maxY, p.y)
            }
            return CGRect(x: minX, y: minY, width: maxX - minX, height: maxY - minY)
        case .step:
            return CGRect(x: start.x - 10, y: start.y - 10, width: 20, height: 20)
        default:
            return CGRect(
                x: min(start.x, end.x),
                y: min(start.y, end.y),
                width: abs(end.x - start.x),
                height: abs(end.y - start.y),
            )
        }
    }

    /// Freehand strokes decimate samples closer than this (points) —
    /// unthrottled mouseDragged delivery would otherwise grow the
    /// point list without bound and every redraw re-strokes all of it.
    static let freehandMinSampleDistance: CGFloat = 1.5

    /// The meaningful minimum; tinier drags count as clicks.
    var isSubstantial: Bool {
        switch tool {
        case .text, .step: true
        case .freehand: points.count >= 2
        default: rect.width >= 3 || rect.height >= 3
        }
    }

    /// A copy shifted by `delta` — used when the selection moves and
    /// the annotations ride along with it.
    func translated(by delta: CGPoint) -> Annotation {
        var copy = self
        copy.start = CGPoint(x: start.x + delta.x, y: start.y + delta.y)
        copy.end = CGPoint(x: end.x + delta.x, y: end.y + delta.y)
        copy.points = points.map { CGPoint(x: $0.x + delta.x, y: $0.y + delta.y) }
        return copy
    }

    /// A copy affine-mapped from rect `src` onto rect `dst` — used when
    /// the selection resizes. Shapes stretch with the region; text and
    /// step markers keep their font/marker size and only reposition
    /// (scaling glyphs mid-gesture reads worse than keeping them).
    /// Mosaic patches are re-baked by the caller after the gesture.
    func mapped(from src: CGRect, to dst: CGRect) -> Annotation {
        guard src.width > 0, src.height > 0 else { return self }
        let sx = dst.width / src.width
        let sy = dst.height / src.height
        func map(_ p: CGPoint) -> CGPoint {
            CGPoint(x: dst.minX + (p.x - src.minX) * sx, y: dst.minY + (p.y - src.minY) * sy)
        }
        var copy = self
        copy.start = map(start)
        copy.end = map(end)
        copy.points = points.map(map)
        return copy
    }

    func draw() {
        color.setStroke()
        switch tool {
        case .rectangle:
            let path = NSBezierPath(rect: rect)
            path.lineWidth = lineWidth
            path.stroke()
        case .ellipse:
            let path = NSBezierPath(ovalIn: rect)
            path.lineWidth = lineWidth
            path.stroke()
        case .line:
            let path = NSBezierPath()
            path.lineWidth = lineWidth
            path.lineCapStyle = .round
            path.move(to: start)
            path.line(to: end)
            path.stroke()
        case .arrow:
            Annotation.strokeArrow(from: start, to: end, lineWidth: lineWidth)
        case .freehand:
            guard points.count >= 2 else { return }
            let path = NSBezierPath()
            path.lineWidth = lineWidth
            path.lineCapStyle = .round
            path.lineJoinStyle = .round
            path.move(to: points[0])
            for p in points.dropFirst() {
                path.line(to: p)
            }
            path.stroke()
        case .text:
            let font = fontName.flatMap { NSFont(name: $0, size: fontSize) }
                ?? NSFont.boldSystemFont(ofSize: fontSize)
            let attributes: [NSAttributedString.Key: Any] = [
                .font: font,
                .foregroundColor: color,
            ]
            text.draw(at: start, withAttributes: attributes)
        case .step:
            Annotation.drawStepMarker(number: number, at: start, color: color)
        case .mosaic:
            patch?.draw(in: rect)
        }
    }

    private static func strokeArrow(from start: CGPoint, to end: CGPoint, lineWidth: CGFloat) {
        let path = NSBezierPath()
        path.lineWidth = lineWidth
        path.lineCapStyle = .round
        path.lineJoinStyle = .round
        path.move(to: start)
        path.line(to: end)
        let angle = atan2(end.y - start.y, end.x - start.x)
        let headLength: CGFloat = 9 + lineWidth * 2
        for delta in [CGFloat.pi * 0.83, -CGFloat.pi * 0.83] {
            path.move(to: end)
            path.line(to: CGPoint(
                x: end.x + headLength * cos(angle + delta),
                y: end.y + headLength * sin(angle + delta),
            ))
        }
        path.stroke()
    }

    /// Numbered marker: filled color circle, bold white
    /// number centered inside.
    private static func drawStepMarker(number: Int, at center: CGPoint, color: NSColor) {
        let radius: CGFloat = 9
        let circleRect = CGRect(
            x: center.x - radius, y: center.y - radius,
            width: radius * 2, height: radius * 2,
        )
        let circle = NSBezierPath(ovalIn: circleRect)
        color.setFill()
        circle.fill()

        let label = "\(number)"
        let font = NSFont.boldSystemFont(ofSize: 11)
        let attributes: [NSAttributedString.Key: Any] = [
            .font: font,
            .foregroundColor: NSColor.white,
        ]
        let size = label.size(withAttributes: attributes)
        label.draw(
            at: CGPoint(x: center.x - size.width / 2, y: center.y - size.height / 2),
            withAttributes: attributes,
        )
    }
}
