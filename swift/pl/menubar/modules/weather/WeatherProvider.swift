import Foundation

/// Selectable weather backend, persisted by raw value.
enum WeatherProviderKind: String, CaseIterable, Sendable {
    case openMeteo
    case qweather

    var label: String {
        switch self {
        case .openMeteo: "Open-Meteo"
        case .qweather: "和风天气"
        }
    }
}

/// A weather backend: city-name geocoding plus a full snapshot fetch.
/// Implementations must be Sendable; fetches happen off the main actor.
protocol WeatherProvider: Sendable {
    /// Resolve a free-form city name into candidate locations, best first.
    func geocode(city: String) async throws -> [WeatherLocation]
    /// Fetch current conditions + 24h hourly + 7d daily for a location.
    func fetch(location: WeatherLocation) async throws -> WeatherSnapshot
}
