@testable import AuraBar
import XCTest

/// Pure calendar-logic tests: month grid math, day keys, solar terms,
/// lunar text, the embedded holiday table, and EventStore's multi-day
/// spanning. No EventKit daemon access (only the static `daysSpanned`).
final class CalendarLogicTests: XCTestCase {
    private let cal = CalendarModel.calendar

    private func date(_ y: Int, _ m: Int, _ d: Int) -> Date {
        guard let date = cal.date(from: DateComponents(year: y, month: m, day: d)) else {
            XCTFail("invalid date \(y)-\(m)-\(d)")
            return Date()
        }
        return date
    }

    // MARK: - YearMonth

    func testYearMonthShiftedAcrossYears() {
        XCTAssertEqual(YearMonth(year: 2026, month: 1).shifted(by: -1), YearMonth(year: 2025, month: 12))
        XCTAssertEqual(YearMonth(year: 2025, month: 12).shifted(by: 1), YearMonth(year: 2026, month: 1))
        XCTAssertEqual(YearMonth(year: 2026, month: 6).shifted(by: -6), YearMonth(year: 2025, month: 12))
        XCTAssertEqual(YearMonth(year: 2026, month: 6).shifted(by: 7), YearMonth(year: 2027, month: 1))
    }

    // MARK: - monthGrid

    func testMonthGridAlways42CellsStartingOnWeekStart() throws {
        for weekStart in [WeekStart.monday, .sunday] {
            for month in [YearMonth(year: 2026, month: 2), YearMonth(year: 2026, month: 9)] {
                let grid = CalendarModel.monthGrid(month, weekStart: weekStart)
                XCTAssertEqual(grid.count, 42)
                XCTAssertEqual(
                    try cal.component(.weekday, from: XCTUnwrap(grid.first).date),
                    weekStart.rawValue,
                )
                // Cells are consecutive calendar days.
                for (a, b) in zip(grid, grid.dropFirst()) {
                    XCTAssertEqual(b.date, cal.date(byAdding: .day, value: 1, to: a.date))
                }
            }
        }
    }

    func testMonthGridCoversWholeDisplayedMonth() {
        // February 2026 (non-leap): exactly 28 in-month cells, 1st–28th.
        let grid = CalendarModel.monthGrid(YearMonth(year: 2026, month: 2), weekStart: .monday)
        let inMonth = grid.filter(\.isInDisplayedMonth)
        XCTAssertEqual(inMonth.count, 28)
        XCTAssertEqual(inMonth.first?.day, 1)
        XCTAssertEqual(inMonth.last?.day, 28)
        XCTAssertEqual(inMonth.first?.date, date(2026, 2, 1))
        XCTAssertEqual(inMonth.last?.date, date(2026, 2, 28))
    }

    // MARK: - Day keys

    func testDayKeyRoundTrip() {
        for (y, m, d) in [(2026, 9, 6), (2025, 12, 31), (2026, 1, 1), (2024, 2, 29)] {
            let key = CalendarModel.dayKey(for: date(y, m, d))
            XCTAssertEqual(key, y * 10000 + m * 100 + d)
            XCTAssertEqual(CalendarModel.date(forDayKey: key), date(y, m, d))
        }
    }

    func testDayKeyShiftedAcrossMonthAndYear() {
        XCTAssertEqual(CalendarModel.dayKeyShifted(from: 20_260_101, by: -1), 20_251_231)
        XCTAssertEqual(CalendarModel.dayKeyShifted(from: 20_251_231, by: 1), 20_260_101)
        XCTAssertEqual(CalendarModel.dayKeyShifted(from: 20_260_228, by: 1), 20_260_301)
        // 2024 is a leap year.
        XCTAssertEqual(CalendarModel.dayKeyShifted(from: 20_240_228, by: 1), 20_240_229)
    }

    // MARK: - SolarTerms

    func testSolarTermLookup() {
        XCTAssertEqual(SolarTerms.term(for: date(2026, 2, 4)), "立春")
        XCTAssertNil(SolarTerms.term(for: date(2026, 2, 5)))
    }

    // MARK: - Lunar

    func testLunarDayTextAroundSpringFestival() {
        // 2025-01-29 is Chinese New Year — lunar 正月初一 shows the
        // month name; the day after is 初二.
        XCTAssertEqual(Lunar.dayText(for: date(2025, 1, 29)), "正月")
        XCTAssertEqual(Lunar.dayText(for: date(2025, 1, 30)), "初二")
    }

    func testLunarTextPrefersSolarTerm() {
        // 2026-02-04 is 立春; text() surfaces the term, dayText() the
        // plain lunar annotation.
        XCTAssertEqual(Lunar.text(for: date(2026, 2, 4)), "立春")
        XCTAssertNotEqual(Lunar.dayText(for: date(2026, 2, 4)), "立春")
    }

    // MARK: - Holidays

    func testHolidayEntry() {
        XCTAssertEqual(
            Holidays.entry(for: date(2026, 9, 25)),
            Holidays.Entry(name: "中秋节", kind: .rest),
        )
        XCTAssertEqual(
            Holidays.entry(for: date(2026, 9, 20)),
            Holidays.Entry(name: "中秋调休", kind: .work),
        )
        XCTAssertNil(Holidays.entry(for: date(2026, 9, 24)))
    }

    func testHolidayCrossYearBlock() {
        // 2026 元旦: 1.1–1.3 rest, 1.4 shifted workday; from late
        // December the countdown already points at 元旦.
        XCTAssertEqual(Holidays.entry(for: date(2026, 1, 3))?.kind, .rest)
        XCTAssertEqual(Holidays.entry(for: date(2026, 1, 4))?.kind, .work)
        XCTAssertEqual(
            Holidays.nextHoliday(from: date(2025, 12, 30))?.name,
            "元旦",
        )
    }

    func testNextHolidayBeforeBlock() throws {
        // 2026-09-06 → next holiday is 中秋节, 9.25–9.27, 3 days.
        let block = try XCTUnwrap(Holidays.nextHoliday(from: date(2026, 9, 6)))
        XCTAssertEqual(block.name, "中秋节")
        XCTAssertEqual(block.start, date(2026, 9, 25))
        XCTAssertEqual(block.end, date(2026, 9, 27))
        XCTAssertEqual(block.days, 3)
    }

    func testNextHolidayInsideBlockReportsWholeBlock() throws {
        let block = try XCTUnwrap(Holidays.nextHoliday(from: date(2026, 9, 26)))
        XCTAssertEqual(block.name, "中秋节")
        XCTAssertEqual(block.start, date(2026, 9, 25))
        XCTAssertEqual(block.end, date(2026, 9, 27))
        XCTAssertEqual(block.days, 3)
    }

    func testIsStatutoryFestival() {
        XCTAssertTrue(Holidays.Entry(name: "中秋节", kind: .rest).isStatutoryFestival)
        XCTAssertTrue(Holidays.Entry(name: "元旦", kind: .rest).isStatutoryFestival)
        XCTAssertFalse(Holidays.Entry(name: "中秋调休", kind: .work).isStatutoryFestival)
        XCTAssertFalse(Holidays.Entry(name: "周末", kind: .rest).isStatutoryFestival)
    }

    // MARK: - EventStore.daysSpanned

    func testDaysSpannedSingleAllDayEvent() {
        // An all-day event "on Friday" ends Saturday 00:00 (exclusive).
        XCTAssertEqual(
            EventStore.daysSpanned(from: date(2026, 9, 4), to: date(2026, 9, 5)),
            [date(2026, 9, 4)],
        )
    }

    func testDaysSpannedMultiDayEvent() {
        XCTAssertEqual(
            EventStore.daysSpanned(from: date(2026, 9, 25), to: date(2026, 9, 28)),
            [date(2026, 9, 25), date(2026, 9, 26), date(2026, 9, 27)],
        )
    }

    func testDaysSpannedTimedEvent() throws {
        let start = try XCTUnwrap(cal.date(from: DateComponents(year: 2026, month: 9, day: 6, hour: 10)))
        let end = try XCTUnwrap(cal.date(from: DateComponents(year: 2026, month: 9, day: 6, hour: 11)))
        XCTAssertEqual(EventStore.daysSpanned(from: start, to: end), [date(2026, 9, 6)])
    }

    func testDaysSpannedOvernightTimedEvent() throws {
        let start = try XCTUnwrap(cal.date(from: DateComponents(year: 2026, month: 9, day: 6, hour: 23)))
        let end = try XCTUnwrap(cal.date(from: DateComponents(year: 2026, month: 9, day: 7, hour: 1)))
        XCTAssertEqual(EventStore.daysSpanned(from: start, to: end), [date(2026, 9, 6), date(2026, 9, 7)])
    }
}
