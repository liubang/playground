import Foundation

/// Chinese statutory holidays and shifted workdays (调休), maintained
/// manually per the State Council's annual announcement. Dates are local
/// start-of-day; entries outside covered years simply render nothing.
enum Holidays {
    enum Kind: Sendable, Equatable {
        /// Statutory holiday or weekend merged into a holiday block — red "休".
        case rest
        /// Weekend day shifted into a workday (补班) — yellow "班".
        case work

        var badge: String {
            switch self {
            case .rest: "休"
            case .work: "班"
            }
        }
    }

    struct Entry: Sendable, Equatable {
        let name: String
        let kind: Kind
    }

    /// Lookup by local start-of-day date. Remote-synced entries (see
    /// HolidaySync) win over the embedded table.
    static func entry(for date: Date) -> Entry? {
        let day = CalendarModel.calendar.startOfDay(for: date)
        return remoteTable[day] ?? table[day]
    }

    /// Override layer installed by HolidaySync; empty until the first
    /// successful sync (cache or network).
    private static var remoteTable: [Date: Entry] = [:]

    static func installRemote(_ entries: [Date: Entry]) {
        remoteTable = entries
    }

    private static let table: [Date: Entry] = buildTable()

    private static func buildTable() -> [Date: Entry] {
        var table: [Date: Entry] = [:]
        var cal = Calendar(identifier: .gregorian)
        cal.locale = Locale(identifier: "zh_CN")

        func put(_ y: Int, _ m: Int, _ d: Int, _ name: String, _ kind: Kind) {
            guard let date = cal.date(from: DateComponents(year: y, month: m, day: d)) else { return }
            table[cal.startOfDay(for: date)] = Entry(name: name, kind: kind)
        }
        func range(_ y1: Int, _ m1: Int, _ d1: Int, _ y2: Int, _ m2: Int, _ d2: Int, _ name: String) {
            guard let start = cal.date(from: DateComponents(year: y1, month: m1, day: d1)),
                  let end = cal.date(from: DateComponents(year: y2, month: m2, day: d2)) else { return }
            var date = start
            while date <= end {
                table[cal.startOfDay(for: date)] = Entry(name: name, kind: .rest)
                guard let next = cal.date(byAdding: .day, value: 1, to: date) else { break }
                date = next
            }
        }

        // MARK: 2024

        range(2023, 12, 30, 2024, 1, 1, "元旦")
        range(2024, 2, 10, 2024, 2, 17, "春节")
        put(2024, 2, 4, "春节调休", .work)
        put(2024, 2, 18, "春节调休", .work)
        range(2024, 4, 4, 2024, 4, 6, "清明节")
        put(2024, 4, 7, "清明调休", .work)
        range(2024, 5, 1, 2024, 5, 5, "劳动节")
        put(2024, 4, 28, "劳动节调休", .work)
        put(2024, 5, 11, "劳动节调休", .work)
        put(2024, 6, 10, "端午节", .rest)
        range(2024, 9, 15, 2024, 9, 17, "中秋节")
        put(2024, 9, 14, "中秋调休", .work)
        range(2024, 10, 1, 2024, 10, 7, "国庆节")
        put(2024, 9, 29, "国庆调休", .work)
        put(2024, 10, 12, "国庆调休", .work)

        // MARK: 2025

        put(2025, 1, 1, "元旦", .rest)
        range(2025, 1, 28, 2025, 2, 4, "春节")
        put(2025, 1, 26, "春节调休", .work)
        put(2025, 2, 8, "春节调休", .work)
        range(2025, 4, 4, 2025, 4, 6, "清明节")
        range(2025, 5, 1, 2025, 5, 5, "劳动节")
        put(2025, 4, 27, "劳动节调休", .work)
        range(2025, 5, 31, 2025, 6, 2, "端午节")
        range(2025, 10, 1, 2025, 10, 8, "国庆节")
        put(2025, 9, 28, "国庆调休", .work)
        put(2025, 10, 11, "国庆调休", .work)

        // MARK: 2026

        range(2026, 1, 1, 2026, 1, 3, "元旦")
        put(2026, 1, 4, "元旦调休", .work)
        range(2026, 2, 15, 2026, 2, 23, "春节")
        put(2026, 2, 14, "春节调休", .work)
        put(2026, 2, 28, "春节调休", .work)
        range(2026, 4, 4, 2026, 4, 6, "清明节")
        range(2026, 5, 1, 2026, 5, 5, "劳动节")
        put(2026, 5, 9, "劳动节调休", .work)
        range(2026, 6, 19, 2026, 6, 21, "端午节")
        range(2026, 9, 25, 2026, 9, 27, "中秋节")
        range(2026, 10, 1, 2026, 10, 7, "国庆节")
        put(2026, 9, 20, "中秋调休", .work)
        put(2026, 10, 10, "国庆调休", .work)

        return table
    }
}
