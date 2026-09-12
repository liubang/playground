import AppKit

/// One display's role in a capture session: its snapshot and its
/// geometry pre-converted into NS space for AppKit hit-testing.
struct DisplayContext {
    let screen: NSScreen
    let frameNS: NSRect
    let snapshot: ScreenSnapshot

    var scale: CGFloat { snapshot.scale }

    /// Maps a selection rect (view/canvas space = screen points, origin
    /// bottom-left) into a pixel crop rect inside `snapshot.image` —
    /// ready for CGImage.cropping, whose y axis runs the other way.
    func cropRectPixels(forSelectionPoints sel: CGRect) -> CGRect {
        let imageHeight = CGFloat(snapshot.image.height)
        let x0 = (sel.minX * scale).rounded(.down)
        let x1 = (sel.maxX * scale).rounded(.up)
        let y0 = (sel.minY * scale).rounded(.down)
        let y1 = (sel.maxY * scale).rounded(.up)
        // Edges round OUTWARD to whole pixels: fractional mouse
        // coordinates must never make the crop size disagree with the
        // compositor's expectations.
        return CGRect(x: x0, y: imageHeight - y1, width: x1 - x0, height: y1 - y0)
            .intersection(snapshot.image.rect)
    }
}

/// The full capture session state handed to every overlay view: all
/// displays plus the suction candidate windows. Pure data — the views
/// do hit-testing against it, mutations flow back through callbacks.
struct Canvas {
    let displays: [DisplayContext]
    let windowCandidates: [WindowCandidate]

    var frameNS: NSRect {
        displays.reduce(.null) { $0.union($1.frameNS) }
    }

    /// The display containing an NS-space point (typically the mouse).
    func display(at pointNS: NSPoint) -> DisplayContext? {
        displays.first { $0.frameNS.contains(pointNS) }
    }

    /// The topmost suction candidate under a CG-space point.
    func windowCandidate(at pointCG: CGPoint) -> WindowCandidate? {
        windowCandidates.first { $0.frameCG.contains(pointCG) }
    }
}

private extension CGImage {
    var rect: CGRect {
        CGRect(x: 0, y: 0, width: width, height: height)
    }
}
