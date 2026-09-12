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
enum CoordinateSpace {
    /// Height of the primary display; the flip pivot between the two
    /// spaces. Computed lazily — NSScreen.main is nil before app launch.
    static var primaryHeight: CGFloat {
        NSScreen.screens.first?.frame.height ?? 0
    }

    static func pointToNS(_ pointCG: CGPoint) -> NSPoint {
        NSPoint(x: pointCG.x, y: primaryHeight - pointCG.y)
    }

    static func pointToCG(_ pointNS: NSPoint) -> CGPoint {
        CGPoint(x: pointNS.x, y: primaryHeight - pointNS.y)
    }

    static func rectToNS(_ rectCG: CGRect) -> NSRect {
        NSRect(
            x: rectCG.minX,
            y: primaryHeight - rectCG.maxY,
            width: rectCG.width,
            height: rectCG.height,
        )
    }

    static func rectToCG(_ rectNS: NSRect) -> CGRect {
        CGRect(
            x: rectNS.minX,
            y: primaryHeight - rectNS.maxY,
            width: rectNS.width,
            height: rectNS.height,
        )
    }
}
