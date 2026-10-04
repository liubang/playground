import Foundation

/// 和风天气 backend: fast in China, requires a free API key from
/// https://dev.qweather.com — paste it in the weather settings section.
struct QWeatherProvider: WeatherProvider {
    let apiKey: String

    private let session: URLSession = {
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 15
        return URLSession(configuration: config)
    }()

    func geocode(city: String) async throws -> [WeatherLocation] {
        guard !apiKey.isEmpty else { throw WeatherError.missingAPIKey }
        var comps = URLComponents(string: "https://geoapi.qweather.com/v2/city/lookup")!
        comps.queryItems = [
            URLQueryItem(name: "location", value: city),
            URLQueryItem(name: "key", value: apiKey),
            URLQueryItem(name: "range", value: "cn"),
        ]
        let (data, _) = try await session.data(from: comps.url!)
        let response = try JSONDecoder().decode(LookupResponse.self, from: data)
        guard response.code == "200" else {
            throw WeatherError.badResponse("city/lookup code=\(response.code)")
        }
        guard let locations = response.location, !locations.isEmpty else {
            throw WeatherError.cityNotFound(city)
        }
        // Skip entries with unparseable coordinates rather than
        // fabricating a location at (0°, 0°) in the Gulf of Guinea.
        return locations.compactMap { r in
            guard let latitude = Double(r.lat), let longitude = Double(r.lon) else { return nil }
            let qualifier = [r.adm1, r.country].first { !$0.isEmpty && $0 != r.name }
            return WeatherLocation(
                name: qualifier.map { "\(r.name) · \($0)" } ?? r.name,
                latitude: latitude,
                longitude: longitude,
            )
        }
    }

    func fetch(location: WeatherLocation) async throws -> WeatherSnapshot {
        guard !apiKey.isEmpty else { throw WeatherError.missingAPIKey }
        // 和风天气接受 "经度,纬度" 形式的坐标。
        let loc = "\(location.longitude),\(location.latitude)"
        async let current = fetchNow(loc)
        async let hourlyResult = fetch24h(loc)
        // fetch7d needs the city's timezone — derived from the hourly
        // fxTime offset — to place its naive sunrise/sunset clock times
        // in the right day, so it waits on the hourly response.
        let (hourly, timeZone) = try await hourlyResult
        async let daily = fetch7d(loc, timeZone: timeZone)
        return try await WeatherSnapshot(
            location: location,
            current: current,
            hourly: hourly,
            daily: daily,
            fetchedAt: Date(),
            airQuality: nil,
        )
    }

    // MARK: - Endpoints

    private func get(_ path: String, query: [URLQueryItem]) async throws -> Data {
        var comps = URLComponents(string: "https://devapi.qweather.com\(path)")!
        comps.queryItems = query + [URLQueryItem(name: "key", value: apiKey)]
        let (data, _) = try await session.data(from: comps.url!)
        return data
    }

    private func fetchNow(_ loc: String) async throws -> CurrentWeather {
        let data = try await get("/v7/weather/now", query: [URLQueryItem(name: "location", value: loc)])
        let response = try JSONDecoder().decode(NowResponse.self, from: data)
        guard response.code == "200" else {
            throw WeatherError.badResponse("weather/now code=\(response.code)")
        }
        return CurrentWeather(
            temperature: Double(response.now.temp) ?? 0,
            apparentTemperature: Double(response.now.feelsLike) ?? 0,
            humidity: Double(response.now.humidity) ?? 0,
            windSpeedKmh: Double(response.now.windSpeed) ?? 0,
            condition: Self.condition(for: response.now.icon),
        )
    }

    /// The hourly points plus the city's timezone (from the fxTime UTC
    /// offset), which the daily parser needs for sunrise/sunset.
    private func fetch24h(_ loc: String) async throws -> ([HourPoint], TimeZone?) {
        let data = try await get("/v7/weather/24h", query: [URLQueryItem(name: "location", value: loc)])
        let response = try JSONDecoder().decode(HourlyResponse.self, from: data)
        guard response.code == "200" else {
            throw WeatherError.badResponse("weather/24h code=\(response.code)")
        }
        let parser = ISO8601DateFormatter()
        let timeZone = response.hourly.first.flatMap { Self.timeZone(fromFxTime: $0.fxTime) }
        let points: [HourPoint] = response.hourly.prefix(24).compactMap { h in
            guard let date = parser.date(from: h.fxTime) else { return nil }
            return HourPoint(
                date: date,
                temperature: Double(h.temp) ?? 0,
                precipProbability: Int(h.pop),
                condition: Self.condition(for: h.icon),
            )
        }
        return (points, timeZone)
    }

    private func fetch7d(_ loc: String, timeZone: TimeZone?) async throws -> [DayForecast] {
        let data = try await get("/v7/weather/7d", query: [URLQueryItem(name: "location", value: loc)])
        let response = try JSONDecoder().decode(DailyResponse.self, from: data)
        guard response.code == "200" else {
            throw WeatherError.badResponse("weather/7d code=\(response.code)")
        }
        let parser = DateFormatter()
        parser.locale = Locale(identifier: "en_US_POSIX")
        parser.dateFormat = "yyyy-MM-dd"
        // fxDate is the city's calendar day; parsing it in the device
        // timezone would shift it when the two differ.
        parser.timeZone = timeZone ?? .current
        return response.daily.compactMap { d in
            guard let date = parser.date(from: d.fxDate) else { return nil }
            return DayForecast(
                date: date,
                condition: Self.condition(for: d.iconDay),
                tempMin: Double(d.tempMin) ?? 0,
                tempMax: Double(d.tempMax) ?? 0,
                precipProbability: Int(d.pop ?? ""),
                sunrise: Self.dayTime(d.sunrise, on: date, timeZone: timeZone),
                sunset: Self.dayTime(d.sunset, on: date, timeZone: timeZone),
            )
        }
    }

    /// The UTC offset embedded in an fxTime string
    /// ("2026-10-04T06:00+08:00") — the only timezone signal QWeather
    /// gives for a location.
    private static func timeZone(fromFxTime fxTime: String) -> TimeZone? {
        let suffix = fxTime.suffix(6)
        guard let sign = suffix.first, sign == "+" || sign == "-" else { return nil }
        let parts = suffix.dropFirst().split(separator: ":")
        guard parts.count == 2, let hours = Int(parts[0]), let minutes = Int(parts[1]) else {
            return nil
        }
        let seconds = (hours * 3600 + minutes * 60) * (sign == "+" ? 1 : -1)
        return TimeZone(secondsFromGMT: seconds)
    }

    /// Combines an "HH:mm" clock string with a day's start **in the
    /// city's timezone** — sunrise/sunset arrive as naive local times,
    /// so Calendar.current would misplace them whenever the device is
    /// not in the queried city. Falls back to the device calendar when
    /// no offset was seen.
    private static func dayTime(_ clock: String?, on day: Date, timeZone: TimeZone?) -> Date? {
        guard let clock else { return nil }
        let parts = clock.split(separator: ":").compactMap { Int($0) }
        guard parts.count == 2 else { return nil }
        var calendar = Calendar.current
        if let timeZone {
            calendar.timeZone = timeZone
        }
        return calendar.date(
            bySettingHour: parts[0],
            minute: parts[1],
            second: 0,
            of: day,
        )
    }

    /// 和风天气 icon codes → unified condition.
    /// https://dev.qweather.com/docs/resource/icons/
    static func condition(for icon: String) -> WeatherCondition {
        switch icon {
        case "100", "150": .clear
        case "101", "102", "103", "151", "152", "153": .partlyCloudy
        case "104", "154": .overcast
        case "300", "301", "350", "351": .rain
        case "302", "303": .thunder
        case "304": .hail
        case "305", "309": .drizzle
        case "306", "307", "314", "315": .rain
        case "308", "310", "311", "312", "316", "317", "318": .heavyRain
        case "313", "404", "405", "406", "456": .sleet
        case "400", "401", "402", "403", "407", "457", "499": .snow
        case "500", "501", "509", "510", "514", "515": .fog
        case "502", "503", "504", "507", "508", "511", "512", "513": .haze
        default: .unknown
        }
    }

    // MARK: - Wire models

    private struct LookupResponse: Decodable {
        struct Entry: Decodable {
            let name: String
            let lat: String
            let lon: String
            let adm1: String
            let country: String
        }

        let code: String
        let location: [Entry]?
    }

    private struct NowResponse: Decodable {
        struct Now: Decodable {
            let temp: String
            let feelsLike: String
            let icon: String
            let humidity: String
            let windSpeed: String
        }

        let code: String
        let now: Now
    }

    private struct HourlyResponse: Decodable {
        struct Hour: Decodable {
            let fxTime: String
            let temp: String
            let icon: String
            let pop: String
        }

        let code: String
        let hourly: [Hour]
    }

    private struct DailyResponse: Decodable {
        struct Day: Decodable {
            let fxDate: String
            let tempMax: String
            let tempMin: String
            let iconDay: String
            let pop: String?
            let sunrise: String?
            let sunset: String?
        }

        let code: String
        let daily: [Day]
    }
}
