import Foundation

/// Open-Meteo backend: free, keyless, WMO weather codes, with a Chinese
/// geocoding API. https://open-meteo.com
struct OpenMeteoProvider: WeatherProvider {
    private let session: URLSession = {
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 15
        return URLSession(configuration: config)
    }()

    func geocode(city: String) async throws -> [WeatherLocation] {
        var comps = URLComponents(string: "https://geocoding-api.open-meteo.com/v1/search")!
        comps.queryItems = [
            URLQueryItem(name: "name", value: city),
            URLQueryItem(name: "count", value: "6"),
            URLQueryItem(name: "language", value: "zh"),
            URLQueryItem(name: "format", value: "json"),
        ]
        let (data, _) = try await session.data(from: comps.url!)
        let response = try JSONDecoder().decode(GeocodeResponse.self, from: data)
        return (response.results ?? []).map { r in
            let qualifier = [r.admin1, r.country].compactMap(\.self).first { $0 != r.name }
            return WeatherLocation(
                name: qualifier.map { "\(r.name) · \($0)" } ?? r.name,
                latitude: r.latitude,
                longitude: r.longitude,
            )
        }
    }

    func fetch(location: WeatherLocation) async throws -> WeatherSnapshot {
        var comps = URLComponents(string: "https://api.open-meteo.com/v1/forecast")!
        comps.queryItems = [
            URLQueryItem(name: "latitude", value: String(location.latitude)),
            URLQueryItem(name: "longitude", value: String(location.longitude)),
            URLQueryItem(
                name: "current",
                value: "temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m",
            ),
            URLQueryItem(name: "hourly", value: "temperature_2m,weather_code,precipitation_probability"),
            URLQueryItem(
                name: "daily",
                value: "weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset",
            ),
            URLQueryItem(name: "timezone", value: "auto"),
            URLQueryItem(name: "forecast_days", value: "7"),
        ]
        // Air quality rides along concurrently; it's best-effort — a
        // failure must not take the main forecast down with it.
        async let airQuality = fetchAirQuality(location: location)
        let (data, _) = try await session.data(from: comps.url!)
        let response = try JSONDecoder().decode(ForecastResponse.self, from: data)

        // With timezone=auto all timestamps are local to the location and
        // carry no offset; parse them in the reported time zone.
        let timeZone = TimeZone(identifier: response.timezone) ?? .current
        let parser = DateFormatter()
        parser.locale = Locale(identifier: "en_US_POSIX")
        parser.dateFormat = "yyyy-MM-dd'T'HH:mm"
        parser.timeZone = timeZone
        let dayParser = DateFormatter()
        dayParser.locale = Locale(identifier: "en_US_POSIX")
        dayParser.dateFormat = "yyyy-MM-dd"
        dayParser.timeZone = timeZone

        guard let currentTime = parser.date(from: response.current.time) else {
            throw WeatherError.badResponse("current.time")
        }
        let current = CurrentWeather(
            temperature: response.current.temperature2m,
            apparentTemperature: response.current.apparentTemperature,
            humidity: response.current.relativeHumidity2m,
            windSpeedKmh: response.current.windSpeed10m,
            condition: Self.condition(for: response.current.weatherCode),
        )

        // Next 24 hourly points starting from the current hour.
        let hours = response.hourly
        let startIndex = hours.time.firstIndex(where: {
            parser.date(from: $0).map { $0 >= currentTime } ?? false
        }) ?? 0
        var hourly: [HourPoint] = []
        for i in startIndex ..< min(startIndex + 24, hours.time.count) {
            guard let date = parser.date(from: hours.time[i]) else { continue }
            hourly.append(HourPoint(
                date: date,
                temperature: hours.temperature2m[i],
                precipProbability: i < hours.precipitationProbability.count
                    ? hours.precipitationProbability[i] : nil,
                condition: Self.condition(for: hours.weatherCode[i]),
            ))
        }

        var daily: [DayForecast] = []
        for i in 0 ..< response.daily.time.count {
            guard let date = dayParser.date(from: response.daily.time[i]) else { continue }
            daily.append(DayForecast(
                date: date,
                condition: Self.condition(for: response.daily.weatherCode[i]),
                tempMin: response.daily.temperature2mMin[i],
                tempMax: response.daily.temperature2mMax[i],
                precipProbability: i < response.daily.precipitationProbabilityMax.count
                    ? response.daily.precipitationProbabilityMax[i] : nil,
                sunrise: i < response.daily.sunrise.count
                    ? parser.date(from: response.daily.sunrise[i]) : nil,
                sunset: i < response.daily.sunset.count
                    ? parser.date(from: response.daily.sunset[i]) : nil,
            ))
        }

        return await WeatherSnapshot(
            location: location,
            current: current,
            hourly: hourly,
            daily: daily,
            fetchedAt: Date(),
            airQuality: airQuality,
        )
    }

    /// US AQI + PM2.5 from the air-quality API. Nil on any failure.
    private func fetchAirQuality(location: WeatherLocation) async -> AirQuality? {
        var comps = URLComponents(string: "https://air-quality-api.open-meteo.com/v1/air-quality")!
        comps.queryItems = [
            URLQueryItem(name: "latitude", value: String(location.latitude)),
            URLQueryItem(name: "longitude", value: String(location.longitude)),
            URLQueryItem(name: "current", value: "us_aqi,pm2_5"),
            URLQueryItem(name: "timezone", value: "auto"),
        ]
        guard let (data, _) = try? await session.data(from: comps.url!),
              let response = try? JSONDecoder().decode(AirQualityResponse.self, from: data),
              let aqi = response.current.usAqi,
              let pm25 = response.current.pm25
        else {
            return nil
        }
        return AirQuality(aqi: Int(aqi.rounded()), pm25: pm25)
    }

    /// WMO weather interpretation codes → unified condition.
    static func condition(for code: Int) -> WeatherCondition {
        switch code {
        case 0, 1: .clear
        case 2: .partlyCloudy
        case 3: .overcast
        case 45, 48: .fog
        case 51, 53, 55, 56, 57: .drizzle
        case 61, 63, 80, 81: .rain
        case 65, 82: .heavyRain
        case 66, 67: .sleet
        case 71, 73, 75, 77, 85, 86: .snow
        case 95: .thunder
        case 96, 99: .hail
        default: .unknown
        }
    }

    // MARK: - Wire models

    private struct AirQualityResponse: Decodable {
        struct Current: Decodable {
            let usAqi: Double?
            let pm25: Double?

            enum CodingKeys: String, CodingKey {
                case usAqi = "us_aqi"
                case pm25 = "pm2_5"
            }
        }

        let current: Current
    }

    private struct GeocodeResponse: Decodable {
        struct Result: Decodable {
            let name: String
            let latitude: Double
            let longitude: Double
            let admin1: String?
            let country: String?
        }

        let results: [Result]?
    }

    private struct ForecastResponse: Decodable {
        struct Current: Decodable {
            let time: String
            let temperature2m: Double
            let relativeHumidity2m: Double
            let apparentTemperature: Double
            let weatherCode: Int
            let windSpeed10m: Double

            enum CodingKeys: String, CodingKey {
                case time
                case temperature2m = "temperature_2m"
                case relativeHumidity2m = "relative_humidity_2m"
                case apparentTemperature = "apparent_temperature"
                case weatherCode = "weather_code"
                case windSpeed10m = "wind_speed_10m"
            }
        }

        struct Hourly: Decodable {
            let time: [String]
            let temperature2m: [Double]
            let weatherCode: [Int]
            let precipitationProbability: [Int?]

            enum CodingKeys: String, CodingKey {
                case time
                case temperature2m = "temperature_2m"
                case weatherCode = "weather_code"
                case precipitationProbability = "precipitation_probability"
            }
        }

        struct Daily: Decodable {
            let time: [String]
            let weatherCode: [Int]
            let temperature2mMax: [Double]
            let temperature2mMin: [Double]
            let precipitationProbabilityMax: [Int?]
            let sunrise: [String]
            let sunset: [String]

            enum CodingKeys: String, CodingKey {
                case time
                case weatherCode = "weather_code"
                case temperature2mMax = "temperature_2m_max"
                case temperature2mMin = "temperature_2m_min"
                case precipitationProbabilityMax = "precipitation_probability_max"
                case sunrise
                case sunset
            }
        }

        let timezone: String
        let current: Current
        let hourly: Hourly
        let daily: Daily
    }
}
