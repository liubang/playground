import AppKit

/// One display's role in a capture session: its snapshot and its
/// geometry pre-converted into NS space for AppKit hit-testing.
struct DisplayContext {
    let screen: NSScreen
    let frameNS: NSRect
    let snapshot: ScreenSnapshot

    var scale: CGFloat {
        snapshot.scale
    }

    /// Maps a selection rect (view/canvas space = screen points, origin
    /// bottom-left) into a pixel crop rect inside `snapshot.image` —
    /// ready for CGImage.cropping, whose y axis runs the other way.
    func cropRectPixels(forSelectionPoints sel: CGRect) -> CGRect {
        Self.cropRectPixels(
            selectionPoints: sel,
            scale: scale,
            imageWidth: snapshot.image.width,
            imageHeight: snapshot.image.height,
        )
    }

    /// Pure form of the crop math, kept static so unit tests can cover
    /// the rounding/clamping without an NSScreen. Edges round OUTWARD
    /// to whole pixels: fractional mouse coordinates must never make
    /// the crop size disagree with the compositor's expectations.
    static func cropRectPixels(
        selectionPoints sel: CGRect,
        scale: CGFloat,
        imageWidth: Int,
        imageHeight: Int,
    ) -> CGRect {
        let imageH = CGFloat(imageHeight)
        let x0 = (sel.minX * scale).rounded(.down)
        let x1 = (sel.maxX * scale).rounded(.up)
        let y0 = (sel.minY * scale).rounded(.down)
        let y1 = (sel.maxY * scale).rounded(.up)
        return CGRect(x: x0, y: imageH - y1, width: x1 - x0, height: y1 - y0)
            .intersection(CGRect(x: 0, y: 0, width: imageWidth, height: imageHeight))
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
