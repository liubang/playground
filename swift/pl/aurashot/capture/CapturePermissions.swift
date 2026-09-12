import AppKit
import CoreGraphics

/// Screen Recording (TCC) permission helpers.
///
/// TCC binds the grant to the app's code signature: install.sh re-signs
/// every build with one stable local identity so the grant survives
/// rebuilds. Without that, each rebuild silently loses access and
/// capture returns nothing.
enum CapturePermissions {
    /// Non-interactive check. Never prompts.
    static func hasAccess() -> Bool {
        CGPreflightScreenCaptureAccess()
    }

    /// Triggers the system prompt at most once; later calls are no-ops
    /// until the user toggles the switch in System Settings.
    static func requestAccess() {
        _ = CGRequestScreenCaptureAccess()
    }

    /// One-shot guidance shown when capture is attempted without the
    /// grant. Activates the app first so the alert doesn't land behind
    /// whatever the user was doing.
    @MainActor
    static func showGuidance() {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "需要屏幕录制权限"
        alert.informativeText = "AuraShot 通过截取屏幕内容来实现截图。请在「系统设置 → 隐私与安全性 → 屏幕录制」中允许 AuraShot，然后重试。"
        alert.alertStyle = .warning
        alert.addButton(withTitle: "打开系统设置")
        alert.addButton(withTitle: "请求授权")
        alert.addButton(withTitle: "取消")
        switch alert.runModal() {
        case .alertFirstButtonReturn:
            if let url = URL(
                string: "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture",
            ) {
                NSWorkspace.shared.open(url)
            }
        case .alertSecondButtonReturn:
            requestAccess()
        default:
            break
        }
    }
}
