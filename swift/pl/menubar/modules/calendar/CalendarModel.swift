import Foundation

/// Which day a calendar week starts on. Raw values match Calendar's
/// weekday components (1 = Sunday … 7 = Saturday).
enum WeekStart: Int, CaseIterable, Sendable {
    case monday = 2
    case sunday = 1

    var label: String {
        switch self {
        case .monday: "周一"
        case .sunday: "周日"
        }
    }
}

/// A year+month pair identifying the month currently shown in the grid.
struct YearMonth: Equatable, Sendable {
    var year: Int
    var month: Int

    static func containing(_ date: Date, calendar: Calendar = CalendarModel.calendar) -> YearMonth {
        YearMonth(
            year: calendar.component(.year, from: date),
            month: calendar.component(.month, from: date),
        )
    }

    var title: String {
        "\(year)年\(month)月"
    }

    func shifted(by delta: Int) -> YearMonth {
        var m = month + delta
        var y = year
        while m > 12 {
            m -= 12; y += 1
        }
        while m < 1 {
            m += 12; y -= 1
        }
        return YearMonth(year: y, month: m)
    }
}

/// One cell in the month grid. `date` is the local start of day, so it
/// doubles as a stable identity and a dictionary key for annotations.
struct CalendarDay: Identifiable, Equatable, Sendable {
    let date: Date
    let day: Int
    let isToday: Bool
    let isInDisplayedMonth: Bool
    let isWeekend: Bool

    var id: Date {
        date
    }
}

/// Pure date math for the month grid. China has no DST, so adding whole
/// days to start-of-day dates is safe here.
enum CalendarModel {
    static let calendar: Calendar = {
        var c = Calendar(identifier: .gregorian)
        c.locale = Locale(identifier: "zh_CN")
        return c
    }()

    /// 42 cells (6 rows × 7 columns) covering the displayed month, padded
    /// with trailing/leading days of the adjacent months.
    static func monthGrid(_ ym: YearMonth, weekStart: WeekStart) -> [CalendarDay] {
        guard let first = calendar.date(from: DateComponents(year: ym.year, month: ym.month, day: 1)),
              let gridStart = calendar.date(
                  byAdding: .day,
                  value: -((calendar.component(.weekday, from: first) - weekStart.rawValue + 7) % 7),
                  to: first,
              ) else { return [] }

        let today = calendar.startOfDay(for: Date())
        return (0 ..< 42).compactMap { i in
            guard let date = calendar.date(byAdding: .day, value: i, to: gridStart) else { return nil }
            return CalendarDay(
                date: date,
                day: calendar.component(.day, from: date),
                isToday: date == today,
                isInDisplayedMonth: calendar.component(.month, from: date) == ym.month
                    && calendar.component(.year, from: date) == ym.year,
                isWeekend: calendar.isDateInWeekend(date),
            )
        }
    }

    /// Short weekday headers in display order, e.g. 一二三四五六日.
    static func weekdaySymbols(weekStart: WeekStart) -> [String] {
        let base = ["日", "一", "二", "三", "四", "五", "六"]
        return (0 ..< 7).map { base[(weekStart.rawValue - 1 + $0) % 7] }
    }

    static func weekdayName(_ date: Date) -> String {
        let names = ["星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"]
        return names[calendar.component(.weekday, from: date) - 1]
    }
}
