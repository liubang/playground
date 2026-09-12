import AppKit

/// One on-screen window's frame, for magnet suction during selection.
struct WindowCandidate {
    /// CG global coordinates (origin: top-left of primary, y down).
    let frameCG: CGRect
    let ownerPID: pid_t
    let ownerName: String
}

/// Enumerates on-screen window geometry via CGWindowList (design
/// doc §3-D5 and §6.3).
///
/// Window bounds are visible without the Screen Recording grant (only
/// window *names* are redacted), so suction candidates can be built
/// unconditionally.
enum WindowGeometry {
    /// On-screen windows that make sensible suction targets,
    /// front-to-back (CGWindowList order IS z-order). System chrome
    /// and our own windows are excluded.
    ///
    /// Layer policy: CGWindowList layers are the raw window-level
    /// numbers, so status-item popovers (NSPopover panels pinned at
    /// statusBar level, CGWindowList layer 25) sit in the same layer
    /// as the status-bar chrome they hang off. The chrome must be
    /// filtered by OWNER, not by layer — status items are hosted by
    /// Control Center, the menu bar by Window Server, the Dock by the
    /// Dock process — because blacklisting layer 25 killed exactly the
    /// status-item popovers we want to suck onto (verified empirically:
    /// live AuraBar/PandoraBar popovers all report layer 25). The
    /// ≥10 000 cap drops screen-saver strata and the weird 2^31-ish
    /// backstop entries.
    static func magnetCandidates() -> [WindowCandidate] {
        let options: CGWindowListOption = [.optionOnScreenOnly, .excludeDesktopElements]
        guard let list = CGWindowListCopyWindowInfo(options, kCGNullWindowID) as? [[CFString: Any]] else {
            return []
        }
        let ownPID = ProcessInfo.processInfo.processIdentifier
        let excludedOwners: Set = [
            "Window Server", "Dock", "Control Center", "Notification Center", "Spotlight", "Siri",
        ]
        return list.compactMap { info in
            guard let layer = info[kCGWindowLayer] as? Int,
                  layer >= 0, layer < 10000,
                  let bounds = info[kCGWindowBounds] as? [String: CGFloat],
                  let pid = info[kCGWindowOwnerPID] as? pid_t,
                  pid != ownPID,
                  let alpha = info[kCGWindowAlpha] as? Double, alpha > 0
            else {
                return nil
            }
            let ownerName = info[kCGWindowOwnerName] as? String ?? ""
            guard !excludedOwners.contains(ownerName) else { return nil }
            let frame = CGRect(
                x: bounds["X"] ?? 0,
                y: bounds["Y"] ?? 0,
                width: bounds["Width"] ?? 0,
                height: bounds["Height"] ?? 0,
            )
            // Skip tiny/implausible windows (menu extras, widgets).
            guard frame.width >= 50, frame.height >= 50 else { return nil }
            return WindowCandidate(
                frameCG: frame,
                ownerPID: pid,
                ownerName: ownerName,
            )
        }
    }
}
