import Foundation

/// Lunar (Chinese calendar) annotations for a Gregorian date, using the
/// system Chinese calendar — no lookup tables needed for day/month
/// names. Solar terms come from an embedded table (see SolarTerms.swift).
enum Lunar {
    private static let chinese: Calendar = {
        var c = Calendar(identifier: .chinese)
        c.locale = Locale(identifier: "zh_CN")
        return c
    }()

    private static let dayNames = [
        "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
        "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
        "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十",
    ]

    private static let monthNames = [
        "正月", "二月", "三月", "四月", "五月", "六月",
        "七月", "八月", "九月", "十月", "冬月", "腊月",
    ]

    /// Short text for the calendar cell subtitle: solar term > lunar month
    /// name (on day 1) > lunar day name.
    static func text(for date: Date) -> String {
        if let term = SolarTerms.term(for: date) {
            return term
        }
        let comps = chinese.dateComponents([.month, .day], from: date)
        let day = comps.day ?? 1
        if day == 1 {
            return monthName(comps)
        }
        return dayNames[max(0, min(day - 1, dayNames.count - 1))]
    }

    /// Long text for the detail footer, e.g. "七月初一 · 立秋".
    static func longText(for date: Date) -> String {
        let comps = chinese.dateComponents([.month, .day], from: date)
        let day = comps.day ?? 1
        let dayText = dayNames[max(0, min(day - 1, dayNames.count - 1))]
        let month = monthName(comps)
        if let term = SolarTerms.term(for: date) {
            return "\(month)\(dayText) · \(term)"
        }
        return "\(month)\(dayText)"
    }

    private static func monthName(_ comps: DateComponents) -> String {
        let month = max(1, min(comps.month ?? 1, monthNames.count))
        let name = monthNames[month - 1]
        return (comps.isLeapMonth ?? false) ? "闰\(name)" : name
    }
}
