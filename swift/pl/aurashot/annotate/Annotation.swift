import AppKit

/// The annotation tools offered by the toolbar (Xnip parity set:
/// shapes, freehand, pixelate, text, numbered step markers).
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

    /// Freehand only: the stroke's sample points.
    var points: [CGPoint] = []
    /// Step only: the 1-based marker number.
    var number: Int = 1
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

    /// The meaningful minimum; tinier drags count as clicks.
    var isSubstantial: Bool {
        switch tool {
        case .text, .step: return true
        case .freehand: return points.count >= 2
        default: return rect.width >= 3 || rect.height >= 3
        }
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
            let attributes: [NSAttributedString.Key: Any] = [
                .font: NSFont.boldSystemFont(ofSize: fontSize),
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

    /// Xnip-style numbered marker: filled color circle, bold white
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
