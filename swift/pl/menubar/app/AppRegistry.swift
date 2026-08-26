import Foundation

/// Shared references to the app-level stores, registered by AppDelegate
/// at launch so the settings window can reach them from anywhere
/// (popover gear menus included).
@MainActor
enum AppRegistry {
    static var clock: MenuBarClock?
    static var weather: WeatherStore?
    /// False on machines without an internal battery (Mac mini,
    /// Mac Studio) — the battery module is not created at all and the
    /// settings window greys out its toggle.
    static var hasBattery = false
}
