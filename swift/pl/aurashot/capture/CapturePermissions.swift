import AppKit
import CoreGraphics

/// Screen Recording (TCC) permission helpers.
///
/// TCC binds the grant to the app's code signature: install.sh re-signs
/// every build with one stable local identity so the grant survives
/// rebuilds. Without that, each rebuild silently loses access and
/// capture returns nothing.
@MainActor
enum CapturePermissions {
    /// Non-interactive check. Never prompts.
    nonisolated static func hasAccess() -> Bool {
        CGPreflightScreenCaptureAccess()
    }

    /// Triggers the system prompt at most once; later calls are no-ops
    /// until the user toggles the switch in System Settings.
    nonisolated static func requestAccess() {
        _ = CGRequestScreenCaptureAccess()
    }

    /// Polls after the guidance dialog sends the user to System
    /// Settings, so flipping the toggle is noticed without a manual
    /// retry (design doc §6.1). macOS may still require a relaunch for
    /// the grant to reach the capture APIs — hence the restart offer.
    private static var pollTask: Task<Void, Never>?

    /// One-shot guidance shown when capture is attempted without the
    /// grant. Activates the app first so the alert doesn't land behind
    /// whatever the user was doing.
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
            startPollingForGrant()
        case .alertSecondButtonReturn:
            requestAccess()
            startPollingForGrant()
        default:
            break
        }
    }

    /// Watches for the grant for a couple of minutes after guidance.
    private static func startPollingForGrant() {
        pollTask?.cancel()
        pollTask = Task { @MainActor in
            for _ in 0 ..< 80 {
                try? await Task.sleep(nanoseconds: 1_500_000_000)
                if Task.isCancelled {
                    return
                }
                if hasAccess() {
                    offerRestart()
                    return
                }
            }
        }
    }

    /// The toggle usually only reaches the capture APIs on next launch.
    private static func offerRestart() {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "已获得屏幕录制权限"
        alert.informativeText = "macOS 一般需要重启应用后授权才会生效。是否立即重启 AuraShot？"
        alert.alertStyle = .informational
        alert.addButton(withTitle: "立即重启")
        alert.addButton(withTitle: "稍后")
        if alert.runModal() == .alertFirstButtonReturn {
            relaunch()
        }
    }

    private static func relaunch() {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/open")
        process.arguments = ["-n", Bundle.main.bundleURL.path]
        try? process.run()
        NSApp.terminate(nil)
    }
}
