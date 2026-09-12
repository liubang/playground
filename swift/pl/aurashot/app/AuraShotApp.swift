import AppKit

/// AuraShot — a menu-bar screenshot tool (Xnip-style), with a local
/// PaddleOCR engine planned on top of cpp/pl/mllm.
///
/// Pure AppKit entry point: the app owns no persistent windows, only a
/// status item and on-demand capture overlays. The delegate must be
/// retained by hand — NSApplication does not retain its delegate.
@main
enum AuraShotMain {
    private static var delegate: AppDelegate?

    @MainActor
    static func main() {
        let app = NSApplication.shared
        let delegate = AppDelegate()
        self.delegate = delegate
        app.delegate = delegate
        app.setActivationPolicy(.accessory)
        app.run()
    }
}
