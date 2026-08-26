import Foundation

/// A resolved place to fetch weather for. Codable so it persists in
/// UserDefaults once resolved from a city name.
struct WeatherLocation: Equatable, Sendable, Codable, Identifiable {
    var name: String
    var latitude: Double
    var longitude: Double

    var id: String {
        "\(latitude),\(longitude)"
    }
}

/// Unified weather condition across providers. Open-Meteo's WMO codes and
/// 和风天气's icon codes both map onto this set.
enum WeatherCondition: String, Sendable, Equatable, Codable {
    case clear
    case partlyCloudy
    case overcast
    case fog
    case haze
    case drizzle
    case rain
    case heavyRain
    case sleet
    case snow
    case thunder
    case hail
    case unknown

    var label: String {
        switch self {
        case .clear: "晴"
        case .partlyCloudy: "多云"
        case .overcast: "阴"
        case .fog: "雾"
        case .haze: "霾"
        case .drizzle: "毛毛雨"
        case .rain: "雨"
        case .heavyRain: "大雨"
        case .sleet: "雨夹雪"
        case .snow: "雪"
        case .thunder: "雷阵雨"
        case .hail: "冰雹"
        case .unknown: "未知"
        }
    }

    /// SF Symbol used in the menu bar and the popover. Rendered as a
    /// template image in the menu bar, tinted per-condition in the popover.
    var symbolName: String {
        switch self {
        case .clear: "sun.max.fill"
        case .partlyCloudy: "cloud.sun.fill"
        case .overcast: "cloud.fill"
        case .fog: "cloud.fog.fill"
        case .haze: "sun.haze.fill"
        case .drizzle: "cloud.drizzle.fill"
        case .rain: "cloud.rain.fill"
        case .heavyRain: "cloud.heavyrain.fill"
        case .sleet: "cloud.sleet.fill"
        case .snow: "cloud.snow.fill"
        case .thunder: "cloud.bolt.rain.fill"
        case .hail: "cloud.hail.fill"
        case .unknown: "questionmark"
        }
    }
}

struct CurrentWeather: Equatable, Sendable {
    /// °C.
    var temperature: Double
    /// °C.
    var apparentTemperature: Double
    /// Percent, 0-100.
    var humidity: Double
    var windSpeedKmh: Double
    var condition: WeatherCondition
}

struct HourPoint: Equatable, Sendable, Identifiable {
    var date: Date
    /// °C.
    var temperature: Double
    /// Percent 0-100, when the provider offers it.
    var precipProbability: Int?
    var condition: WeatherCondition

    var id: Date {
        date
    }
}

struct DayForecast: Equatable, Sendable, Identifiable {
    var date: Date
    var condition: WeatherCondition
    /// °C.
    var tempMin: Double
    /// °C.
    var tempMax: Double
    /// Percent 0-100, when the provider offers it (Open-Meteo only).
    var precipProbability: Int?
    var sunrise: Date?
    var sunset: Date?

    var id: Date {
        date
    }
}

/// One full fetch result: current conditions, the next 24 hourly points
/// and a 7-day outlook.
struct WeatherSnapshot: Equatable, Sendable {
    var location: WeatherLocation
    var current: CurrentWeather
    var hourly: [HourPoint]
    var daily: [DayForecast]
    var fetchedAt: Date
}

enum WeatherError: LocalizedError {
    case missingAPIKey
    case cityNotFound(String)
    case badResponse(String)

    var errorDescription: String? {
        switch self {
        case .missingAPIKey:
            "请先在设置中填写和风天气 API Key"
        case let .cityNotFound(city):
            "找不到城市「\(city)」"
        case let .badResponse(detail):
            "天气服务返回异常：\(detail)"
        }
    }
}
