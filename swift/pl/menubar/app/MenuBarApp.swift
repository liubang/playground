import AppKit

/// AuraBar — a lightweight, modular menu bar app.
///
/// Pure AppKit entry point: the app owns no windows, only per-module
/// status items (see StatusItemController). The delegate must be retained
/// by hand — NSApplication does not retain its delegate.
@main
enum AuraBarMain {
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
