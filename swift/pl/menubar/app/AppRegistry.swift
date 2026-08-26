import Foundation

/// Shared references to the app-level stores, registered by AppDelegate
/// at launch so the settings window can reach them from anywhere
/// (popover gear menus included).
@MainActor
enum AppRegistry {
    static var clock: MenuBarClock?
    static var weather: WeatherStore?
}
