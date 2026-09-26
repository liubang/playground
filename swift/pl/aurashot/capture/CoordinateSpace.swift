import AppKit

/// Conversions between the two global coordinate spaces used during a
/// capture session (design doc §6.2):
///
///   CG space — origin at the top-left of the primary display, y down.
///              Used by CGWindowList, ScreenCaptureKit, CGEvent.
///   NS space — origin at the bottom-left of the primary display, y up.
///              Used by NSScreen.frame and every AppKit window.
///
/// Mouse locations are read in CG space (NSEvent.mouseLocation maps
/// straight across), so all hit-testing — window magnet suction,
/// overlay picking — happens in CG space and results are converted
/// once, at the boundary where AppKit needs points.
///
/// The math lives in the `primaryHeight:`-taking overloads so unit
/// tests can cover it (multi-display layouts, negative coordinates)
/// without an NSScreen; the convenience wrappers just feed it the live
/// primary height.
enum CoordinateSpace {
    /// Height of the primary display; the flip pivot between the two
    /// spaces. Computed lazily — NSScreen.main is nil before app launch.
    static var primaryHeight: CGFloat {
        NSScreen.screens.first?.frame.height ?? 0
    }

    static func pointToNS(_ pointCG: CGPoint) -> NSPoint {
        pointToNS(pointCG, primaryHeight: primaryHeight)
    }

    static func pointToCG(_ pointNS: NSPoint) -> CGPoint {
        pointToCG(pointNS, primaryHeight: primaryHeight)
    }

    static func rectToNS(_ rectCG: CGRect) -> NSRect {
        rectToNS(rectCG, primaryHeight: primaryHeight)
    }

    static func rectToCG(_ rectNS: NSRect) -> CGRect {
        rectToCG(rectNS, primaryHeight: primaryHeight)
    }

    // MARK: - Pure forms (unit-testable without NSScreen)

    static func pointToNS(_ pointCG: CGPoint, primaryHeight: CGFloat) -> NSPoint {
        NSPoint(x: pointCG.x, y: primaryHeight - pointCG.y)
    }

    static func pointToCG(_ pointNS: NSPoint, primaryHeight: CGFloat) -> CGPoint {
        CGPoint(x: pointNS.x, y: primaryHeight - pointNS.y)
    }

    static func rectToNS(_ rectCG: CGRect, primaryHeight: CGFloat) -> NSRect {
        NSRect(
            x: rectCG.minX,
            y: primaryHeight - rectCG.maxY,
            width: rectCG.width,
            height: rectCG.height,
        )
    }

    static func rectToCG(_ rectNS: NSRect, primaryHeight: CGFloat) -> CGRect {
        CGRect(
            x: rectNS.minX,
            y: primaryHeight - rectNS.maxY,
            width: rectNS.width,
            height: rectNS.height,
        )
    }
}
