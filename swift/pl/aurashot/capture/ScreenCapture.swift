import AppKit
import ScreenCaptureKit

/// One screen's frozen snapshot for a capture session.
struct ScreenSnapshot {
    /// The captured image, at native pixel resolution.
    let image: CGImage
    /// The display this image belongs to.
    let displayID: CGDirectDisplayID
    /// The display's frame in CG global coordinates (origin: top-left
    /// of the primary display, y down) — the same space ScreenCaptureKit
    /// reports frames in.
    let frameCG: CGRect
    /// points → pixels multiplier for this display.
    let scale: CGFloat

    var sizePixels: CGSize {
        CGSize(width: image.width, height: image.height)
    }
}

enum ScreenCaptureError: Error {
    case noDisplays
    case captureFailed(display: CGDirectDisplayID, underlying: Error?)
}

/// Whole-screen snapshots via SCScreenshotManager (macOS 14+).
///
/// Design doc §3-D2: SCKit is the primary path; the deprecated
/// CGWindowListCreateImage fallback is deliberately not implemented —
/// on macOS 14+ both fail identically without the TCC grant, and the
/// permission gate in CaptureSessionController runs before this.
enum ScreenCapture {
    /// Captures every connected display.
    ///
    /// `excludedWindowNumbers`: CGWindow numbers of our own overlay
    /// windows — SCKit compositing skips them, so the snapshot shows
    /// the desktop exactly as it looked before the session began
    /// (no dim veil, no hint label).
    static func snapshotAllDisplays(
        excludingWindowNumbers: Set<Int>,
    ) async throws -> [ScreenSnapshot] {
        let content = try await SCShareableContent.current
        let ownWindows = content.windows.filter {
            excludingWindowNumbers.contains(Int($0.windowID))
        }

        // Resolve backing scales up front: NSScreen is main-thread
        // state, and the capture tasks below run off it.
        let jobs: [(display: SCDisplay, scale: CGFloat)] = content.displays.map {
            ($0, backingScale(for: $0.displayID))
        }

        // Capture all displays CONCURRENTLY — SCScreenshotManager is
        // async and independent per display; a dual-screen setup would
        // otherwise pay two sequential compositing round-trips.
        let snapshots = try await withThrowingTaskGroup(of: ScreenSnapshot.self) { group in
            for (display, scale) in jobs {
                group.addTask {
                    let filter = SCContentFilter(
                        display: display,
                        excludingWindows: ownWindows,
                    )
                    // Capture at the display's native pixel size, not
                    // its point size, so crops stay sharp on Retina.
                    let config = SCStreamConfiguration()
                    config.width = Int(display.frame.width * scale)
                    config.height = Int(display.frame.height * scale)
                    config.showsCursor = false
                    do {
                        let image = try await SCScreenshotManager.captureImage(
                            contentFilter: filter,
                            configuration: config,
                        )
                        return ScreenSnapshot(
                            image: image,
                            displayID: display.displayID,
                            frameCG: display.frame,
                            scale: CGFloat(image.width) / display.frame.width,
                        )
                    } catch {
                        throw ScreenCaptureError.captureFailed(
                            display: display.displayID,
                            underlying: error,
                        )
                    }
                }
            }
            var result: [ScreenSnapshot] = []
            for try await snapshot in group {
                result.append(snapshot)
            }
            return result
        }
        guard !snapshots.isEmpty else { throw ScreenCaptureError.noDisplays }
        return snapshots
    }

    /// AppKit's backingScaleFactor for a CG display; 2.0 is the safe
    /// guess when the display isn't attached to an NSScreen (shouldn't
    /// happen for connected displays).
    private static func backingScale(for displayID: CGDirectDisplayID) -> CGFloat {
        for screen in NSScreen.screens {
            if let number = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? CGDirectDisplayID,
               number == displayID
            {
                return screen.backingScaleFactor
            }
        }
        return 2.0
    }
}
