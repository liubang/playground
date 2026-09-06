import Foundation

/// Chinese statutory holidays and shifted workdays (调休), maintained
/// manually per the State Council's annual announcement. Entries are
/// keyed by `yyyymmdd` day keys (see CalendarModel.dayKey), not Dates —
/// day keys are timezone-agnostic, so the embedded and remote-synced
/// tables stay valid when the user crosses time zones mid-session.
/// Dates outside covered years simply render nothing.
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

        /// A statutory festival day (元旦 / 春节 / X节) — the grid shows
        /// the festival name in rest red as the day's headline annotation.
        var isStatutoryFestival: Bool {
            kind == .rest && (name.hasSuffix("节") || ["元旦", "春节", "除夕"].contains(name))
        }
    }

    // MARK: - Remote override layer

    /// Guards every mutable static below (remote table + derived caches).
    /// Lookups run on background rebuilds while installs happen on main.
    private static let lock = NSLock()

    /// Override layer installed by HolidaySync; empty until the first
    /// successful sync (cache or network).
    private static var remoteTable: [Int: Entry] = [:]

    /// Merged view (remote wins) plus the sorted rest-day key list used
    /// by nextHoliday; rebuilt lazily after each install.
    private static var mergedCache: (table: [Int: Entry], restDays: [Int])?

    static func installRemote(_ entries: [Int: Entry]) {
        lock.lock()
        remoteTable = entries
        mergedCache = nil
        lock.unlock()
    }

    /// Caller must hold `lock`.
    private static func merged() -> (table: [Int: Entry], restDays: [Int]) {
        if let mergedCache {
            return mergedCache
        }
        var merged = table
        merged.merge(remoteTable) { _, remote in remote }
        let result = (merged, merged.filter { $0.value.kind == .rest }.keys.sorted())
        mergedCache = result
        return result
    }

    // MARK: - Lookups

    /// Lookup by date (any time of day); remote-synced entries win over
    /// the embedded table.
    static func entry(for date: Date) -> Entry? {
        let key = CalendarModel.dayKey(for: date)
        lock.lock()
        defer { lock.unlock() }
        return merged().table[key]
    }

    /// A consecutive run of rest days sharing one name.
    struct HolidayBlock: Equatable, Sendable {
        let name: String
        let start: Date
        let end: Date

        var days: Int {
            (CalendarModel.calendar.dateComponents([.day], from: start, to: end).day ?? 0) + 1
        }
    }

    /// The holiday block `date` falls inside of, or the next one after
    /// it. Rest-kind entries only — shifted workdays are ignored.
    static func nextHoliday(from date: Date) -> HolidayBlock? {
        let cal = CalendarModel.calendar
        let today = CalendarModel.dayKey(for: cal.startOfDay(for: date))
        lock.lock()
        defer { lock.unlock() }
        let merged = merged()
        guard let first = merged.restDays.first(where: { $0 >= today }) else { return nil }

        let name = merged.table[first]?.name ?? ""
        // If today sits inside a block, walk back to its first day.
        var startKey = first
        while let prev = CalendarModel.dayKeyShifted(from: startKey, by: -1),
              merged.table[prev]?.name == name, merged.table[prev]?.kind == .rest
        {
            startKey = prev
        }
        var endKey = first
        while let next = CalendarModel.dayKeyShifted(from: endKey, by: 1),
              merged.table[next]?.name == name, merged.table[next]?.kind == .rest
        {
            endKey = next
        }
        guard let start = CalendarModel.date(forDayKey: startKey),
              let end = CalendarModel.date(forDayKey: endKey) else { return nil }
        return HolidayBlock(name: name, start: start, end: end)
    }
}

// MARK: - Embedded table

extension Holidays {
    private static let table: [Int: Entry] = buildTable()

    private static func buildTable() -> [Int: Entry] {
        var table: [Int: Entry] = [:]

        func key(_ y: Int, _ m: Int, _ d: Int) -> Int {
            y * 10000 + m * 100 + d
        }
        func put(_ y: Int, _ m: Int, _ d: Int, _ name: String, _ kind: Kind) {
            table[key(y, m, d)] = Entry(name: name, kind: kind)
        }
        func range(_ y1: Int, _ m1: Int, _ d1: Int, _ y2: Int, _ m2: Int, _ d2: Int, _ name: String) {
            var current = key(y1, m1, d1)
            let end = key(y2, m2, d2)
            while current <= end {
                table[current] = Entry(name: name, kind: .rest)
                guard let next = CalendarModel.dayKeyShifted(from: current, by: 1) else { break }
                current = next
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
