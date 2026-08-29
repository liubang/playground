import SwiftUI

/// A Grafana-style time-series chart drawn with SwiftUI Canvas:
/// horizontal grid lines with value labels on the left, vertical grid
/// lines with time labels at the bottom, smooth (clamped Catmull-Rom)
/// curves with optional gradient area fill.
///
/// Why Canvas instead of Swift Charts: live sliding-window data redraws
/// every sample, and Swift Charts' animation model plus its edge
/// overshoot produce visible artifacts in that scenario. Canvas redraws
/// deterministically — what you compute is exactly what you get.
struct TimeSeriesChart: View {
    struct Series {
        let color: Color
        let values: [Double]
        /// Draw a gradient area fill under the line.
        let fill: Bool

        init(color: Color, values: [Double], fill: Bool = false) {
            self.color = color
            self.values = values
            self.fill = fill
        }
    }

    let series: [Series]
    let maxY: Double
    let yLabel: (Double) -> String
    /// Bottom time labels, drawn at even spacing across the plot.
    let xLabels: [String]

    @Environment(\.theme) private var theme

    private let yGutter: CGFloat = 30
    private let xGutter: CGFloat = 12

    var body: some View {
        Canvas { context, size in
            let plot = CGRect(
                x: yGutter,
                y: 0,
                width: size.width - yGutter,
                height: size.height - xGutter,
            )
            drawGrid(context: context, plot: plot)
            context.drawLayer { layer in
                layer.clip(to: Path(plot))
                for item in series {
                    drawSeries(item, context: layer, plot: plot)
                }
            }
            drawXLabels(context: context, plot: plot)
        }
        .frame(height: 78)
    }

    // MARK: - Grid & labels

    private func drawGrid(context: GraphicsContext, plot: CGRect) {
        let gridColor = theme.cardBorder.opacity(0.55)
        let tickCount = 4
        for i in 0 ..< tickCount {
            let fraction = Double(i) / Double(tickCount - 1)
            let y = plot.maxY - plot.height * fraction
            var line = Path()
            line.move(to: CGPoint(x: plot.minX, y: y))
            line.addLine(to: CGPoint(x: plot.maxX, y: y))
            context.stroke(line, with: .color(gridColor), lineWidth: 0.5)
            // Labels are vertically centered on their grid line; clamp
            // the top one so its upper half isn't clipped by the frame.
            context.draw(
                Text(yLabel(maxY * fraction))
                    .font(.system(size: 8))
                    .foregroundStyle(theme.textSecondary),
                at: CGPoint(x: yGutter - 4, y: max(y, 6)),
                anchor: .trailing,
            )
        }
        let count = xLabels.count
        guard count > 1 else { return }
        for i in 0 ..< count {
            let fraction = Double(i) / Double(count - 1)
            let x = plot.minX + plot.width * fraction
            var line = Path()
            line.move(to: CGPoint(x: x, y: plot.minY))
            line.addLine(to: CGPoint(x: x, y: plot.maxY))
            context.stroke(line, with: .color(gridColor), lineWidth: 0.5)
        }
    }

    private func drawXLabels(context: GraphicsContext, plot: CGRect) {
        let count = xLabels.count
        guard count > 1 else { return }
        for (i, label) in xLabels.enumerated() {
            let fraction = Double(i) / Double(count - 1)
            let x = plot.minX + plot.width * fraction
            let anchor: UnitPoint = switch i {
            case 0: .topLeading
            case count - 1: .topTrailing
            default: .top
            }
            context.draw(
                Text(label)
                    .font(.system(size: 8))
                    .foregroundStyle(theme.textSecondary),
                at: CGPoint(x: x, y: plot.maxY + 2),
                anchor: anchor,
            )
        }
    }

    // MARK: - Series

    private func drawSeries(_ item: Series, context: GraphicsContext, plot: CGRect) {
        let pts = points(for: item.values, in: plot)
        guard pts.count > 1 else { return }

        if item.fill {
            var area = smoothPath(pts, plot: plot)
            area.addLine(to: CGPoint(x: pts[pts.count - 1].x, y: plot.maxY))
            area.addLine(to: CGPoint(x: pts[0].x, y: plot.maxY))
            area.closeSubpath()
            context.fill(
                area,
                with: .linearGradient(
                    Gradient(colors: [item.color.opacity(0.22), item.color.opacity(0.02)]),
                    startPoint: CGPoint(x: 0, y: plot.minY),
                    endPoint: CGPoint(x: 0, y: plot.maxY),
                ),
            )
        }
        context.stroke(
            smoothPath(pts, plot: plot),
            with: .color(item.color),
            style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round),
        )
    }

    private func points(for values: [Double], in plot: CGRect) -> [CGPoint] {
        guard values.count > 1 else { return [] }
        let span = max(maxY, .leastNonzeroMagnitude)
        return values.enumerated().map { index, value in
            let x = plot.minX + plot.width * CGFloat(index) / CGFloat(values.count - 1)
            let clamped = min(max(value, 0), maxY)
            let y = plot.maxY - plot.height * (clamped / span)
            return CGPoint(x: x, y: y)
        }
    }

    /// Catmull-Rom → cubic Bézier with control points clamped to the
    /// plot, so a burst at the window edge can't overshoot above/below
    /// the data range (the classic "hook" artifact).
    private func smoothPath(_ pts: [CGPoint], plot: CGRect) -> Path {
        var path = Path()
        guard let first = pts.first else { return path }
        path.move(to: first)
        for i in 0 ..< (pts.count - 1) {
            let p0 = pts[max(i - 1, 0)]
            let p1 = pts[i]
            let p2 = pts[i + 1]
            let p3 = pts[min(i + 2, pts.count - 1)]
            let c1 = clamped(
                CGPoint(x: p1.x + (p2.x - p0.x) / 6, y: p1.y + (p2.y - p0.y) / 6),
                in: plot,
            )
            let c2 = clamped(
                CGPoint(x: p2.x - (p3.x - p1.x) / 6, y: p2.y - (p3.y - p1.y) / 6),
                in: plot,
            )
            path.addCurve(to: p2, control1: c1, control2: c2)
        }
        return path
    }

    private func clamped(_ point: CGPoint, in rect: CGRect) -> CGPoint {
        CGPoint(
            x: min(max(point.x, rect.minX), rect.maxX),
            y: min(max(point.y, rect.minY), rect.maxY),
        )
    }
}
