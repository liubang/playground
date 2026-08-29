import Foundation

/// Refreshes the statutory-holiday table from the community-maintained
/// holiday-cn dataset (NateScarlet/holiday-cn — updated within days of
/// each year's State Council announcement), so the calendar never goes
/// stale when the embedded table's year range runs out.
///
/// Strategy: fetch current + next year's JSON at most once per day,
/// cache the raw entries in UserDefaults, install them as an override
/// layer on top of the embedded table. Network failures are silent —
/// the embedded table always remains as the offline fallback.
@MainActor
final class HolidaySync: ObservableObject {
    /// Bumped after a new table is installed; the popover rebuilds its
    /// grid so badges/rest coloring pick up the fresh data.
    @Published private(set) var revision = 0

    private static let lastFetchKey = "AuraBar.calendar.holidaySyncAt"
    private static let cacheKey = "AuraBar.calendar.holidayCache"
    private static let minimumInterval: TimeInterval = 24 * 3600

    private let session: URLSession = {
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 10
        return URLSession(configuration: config)
    }()

    init() {
        applyCache()
        Task { await refreshIfStale() }
    }

    // MARK: - Cache

    private func applyCache() {
        guard let data = UserDefaults.standard.data(forKey: Self.cacheKey),
              let days = try? JSONDecoder().decode([RemoteDay].self, from: data),
              !days.isEmpty else { return }
        Holidays.installRemote(Self.makeTable(days))
        revision += 1
    }

    // MARK: - Fetch

    private func refreshIfStale() async {
        let last = UserDefaults.standard.object(forKey: Self.lastFetchKey) as? Date ?? .distantPast
        guard Date().timeIntervalSince(last) >= Self.minimumInterval else { return }

        let year = CalendarModel.calendar.component(.year, from: Date())
        var days: [RemoteDay] = []
        for y in [year, year + 1] {
            guard let url = URL(
                string: "https://cdn.jsdelivr.net/gh/NateScarlet/holiday-cn@master/\(y).json",
            ) else { continue }
            guard let (data, _) = try? await session.data(from: url),
                  let payload = try? JSONDecoder().decode(Payload.self, from: data) else { continue }
            days.append(contentsOf: payload.days)
        }
        guard !days.isEmpty else { return }

        Holidays.installRemote(Self.makeTable(days))
        if let data = try? JSONEncoder().encode(days) {
            UserDefaults.standard.set(data, forKey: Self.cacheKey)
        }
        UserDefaults.standard.set(Date(), forKey: Self.lastFetchKey)
        revision += 1
    }

    // MARK: - Decoding

    private static func makeTable(_ days: [RemoteDay]) -> [Date: Holidays.Entry] {
        var table: [Date: Holidays.Entry] = [:]
        for day in days {
            guard let date = parse(day.date) else { continue }
            table[date] = Holidays.Entry(name: day.name, kind: day.isOffDay ? .rest : .work)
        }
        return table
    }

    /// "2026-01-01" → local start of day.
    private static func parse(_ ymd: String) -> Date? {
        let parts = ymd.split(separator: "-").compactMap { Int($0) }
        guard parts.count == 3 else { return nil }
        return CalendarModel.calendar
            .date(from: DateComponents(year: parts[0], month: parts[1], day: parts[2]))
            .map { CalendarModel.calendar.startOfDay(for: $0) }
    }

    /// One entry in the holiday-cn dataset; also the UserDefaults cache
    /// payload (concatenated years).
    private struct RemoteDay: Codable {
        let name: String
        /// yyyy-MM-dd.
        let date: String
        /// true = day off (休), false = shifted workday (班).
        let isOffDay: Bool
    }

    /// holiday-cn per-year file format.
    private struct Payload: Decodable {
        let days: [RemoteDay]
    }
}
