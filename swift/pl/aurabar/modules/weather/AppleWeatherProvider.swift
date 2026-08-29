import CoreLocation
import Foundation
import WeatherKit

/// Apple WeatherKit backend: the same service as the system Weather
/// app, via the native framework — no HTTP, no API key, and the data
/// set (current/hourly/daily) matches what this app displays one-to-one.
///
/// Two hard requirements, both Apple-enforced:
///   1. the com.apple.weatherkit entitlement (bundled in
///      resources/AuraBar.entitlements), and
///   2. an App ID with the WeatherKit capability enabled under a paid
///      Apple Developer account — the service authenticates server-side
///      against the team, so a local self-signed signature satisfies
///      (1) but not (2). Fetches then fail with a permission error,
///      which we surface as a readable message.
///
/// Geocoding is CoreLocation's CLGeocoder (WeatherKit has no search
/// API). No air quality — WeatherKit doesn't offer one.
struct AppleWeatherProvider: WeatherProvider {
    private let service = WeatherService.shared

    func geocode(city: String) async throws -> [WeatherLocation] {
        let placemarks = try await CLGeocoder().geocodeAddressString(
            city,
            in: nil,
            preferredLocale: Locale(identifier: "zh_CN"),
        )
        return placemarks.prefix(6).compactMap { placemark in
            guard let coordinate = placemark.location?.coordinate else { return nil }
            let name = placemark.locality ?? placemark.name ?? city
            let qualifier = [placemark.administrativeArea, placemark.country]
                .compactMap(\.self)
                .first { $0 != name }
            return WeatherLocation(
                name: qualifier.map { "\(name) · \($0)" } ?? name,
                latitude: coordinate.latitude,
                longitude: coordinate.longitude,
            )
        }
    }

    func fetch(location: WeatherLocation) async throws -> WeatherSnapshot {
        let weather: Weather
        do {
            weather = try await service.weather(for: CLLocation(
                latitude: location.latitude,
                longitude: location.longitude,
            ))
        } catch {
            // Typically WeatherDaemon permission errors when the App ID
            // lacks the WeatherKit capability — the hint matters more
            // than the raw error.
            throw WeatherError.appleUnavailable(error.localizedDescription)
        }

        let now = Date()
        let current = CurrentWeather(
            temperature: Self.celsius(weather.currentWeather.temperature),
            apparentTemperature: Self.celsius(weather.currentWeather.apparentTemperature),
            humidity: weather.currentWeather.humidity * 100,
            windSpeedKmh: weather.currentWeather.wind.speed
                .converted(to: .kilometersPerHour).value,
            condition: Self.condition(for: weather.currentWeather.condition),
        )

        let hourly = weather.hourlyForecast
            .filter { $0.date >= now }
            .prefix(24)
            .map { hour in
                HourPoint(
                    date: hour.date,
                    temperature: Self.celsius(hour.temperature),
                    precipProbability: Int((hour.precipitationChance * 100).rounded()),
                    condition: Self.condition(for: hour.condition),
                )
            }

        let daily = weather.dailyForecast.prefix(7).map { day in
            DayForecast(
                date: day.date,
                condition: Self.condition(for: day.condition),
                tempMin: Self.celsius(day.lowTemperature),
                tempMax: Self.celsius(day.highTemperature),
                precipProbability: Int((day.precipitationChance * 100).rounded()),
                sunrise: day.sun.sunrise,
                sunset: day.sun.sunset,
            )
        }

        return WeatherSnapshot(
            location: location,
            current: current,
            hourly: Array(hourly),
            daily: Array(daily),
            fetchedAt: now,
            airQuality: nil,
        )
    }

    private static func celsius(_ measurement: Measurement<UnitTemperature>) -> Double {
        measurement.converted(to: .celsius).value
    }

    /// WeatherKit conditions → unified condition. WeatherKit splits
    /// finer than we display (mostlyClear vs clear, isolated vs
    /// scattered thunderstorms); collapse onto the nearest bucket.
    static func condition(for condition: WeatherKit.WeatherCondition) -> WeatherCondition {
        switch condition {
        case .clear, .mostlyClear, .hot, .frigid, .breezy, .windy:
            .clear
        case .partlyCloudy:
            .partlyCloudy
        case .mostlyCloudy, .cloudy:
            .overcast
        case .foggy:
            .fog
        case .haze, .smoky, .blowingDust:
            .haze
        case .drizzle, .freezingDrizzle:
            .drizzle
        case .rain, .sunShowers:
            .rain
        case .heavyRain, .tropicalStorm, .hurricane:
            .heavyRain
        case .freezingRain, .sleet, .wintryMix:
            .sleet
        case .snow, .heavySnow, .flurries, .blowingSnow, .sunFlurries, .blizzard:
            .snow
        case .thunderstorms, .isolatedThunderstorms, .scatteredThunderstorms, .strongStorms:
            .thunder
        case .hail:
            .hail
        @unknown default:
            .unknown
        }
    }
}
