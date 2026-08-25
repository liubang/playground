import AppKit
import EventKit
import SwiftUI

/// The popover shown when the calendar menu bar item is clicked: a month
/// grid with lunar / holiday annotations, the selected day's system
/// calendar events, and a compact footer with a settings menu.
struct CalendarPopover: View {
    @ObservedObject var clock: MenuBarClock
    @ObservedObject var eventStore: EventStore
    @ObservedObject var holidaySync: HolidaySync

    @AppStorage("themePreference") private var themePreference = ThemePreference.system.rawValue
    @AppStorage("AuraBar.calendar.weekStart") private var weekStartRaw = WeekStart.monday.rawValue
    @AppStorage("AuraBar.calendar.showLunar") private var showLunar = true
    @Environment(\.colorScheme) private var colorScheme

    @State private var displayed = YearMonth.containing(Date())
    @State private var selected = CalendarModel.calendar.startOfDay(for: Date())
    @State private var settingsError: String?
    /// Precomputed cell view models; rebuilt only when an input changes
    /// (month, week start, lunar toggle) — never on routine re-renders
    /// like selection or the menu bar clock tick.
    @State private var cells: [DayCellData] = []
    /// Year/month quick picker state; replaces the grid while active.
    @State private var picking = false
    @State private var pickerYear = YearMonth.containing(Date()).year

    private var theme: Theme {
        (ThemePreference(rawValue: themePreference) ?? .system).theme(for: colorScheme)
    }

    private var pinnedColorScheme: ColorScheme? {
        (ThemePreference(rawValue: themePreference) ?? .system).pinnedColorScheme
    }

    private var weekStart: WeekStart {
        WeekStart(rawValue: weekStartRaw) ?? .monday
    }

    /// Rebuild the precomputed cell models from all inputs. Lunar text is
    /// the expensive part (Chinese calendar conversion), so it happens
    /// exactly here — 42 conversions per input change, not per render.
    private func rebuildCells() {
        let grid = CalendarModel.monthGrid(displayed, weekStart: weekStart)
        // Dot markers need one range query covering the whole grid.
        let eventDays: Set<Date> = {
            guard let first = grid.first?.date, let last = grid.last?.date,
                  let end = CalendarModel.calendar.date(byAdding: .day, value: 1, to: last)
            else { return [] }
            return eventStore.eventDays(from: first, to: end)
        }()
        cells = grid.map { day in
            let entry = Holidays.entry(for: day.date)
            let isFestival = entry.map {
                $0.kind == .rest && ($0.name.hasSuffix("节") || ["元旦", "春节", "除夕"].contains($0.name))
            } ?? false
            let term = SolarTerms.term(for: day.date)
            let isRestColored = entry?.kind == .work ? false : (entry?.kind == .rest || day.isWeekend)
            return DayCellData(
                date: day.date,
                day: day.day,
                isToday: day.isToday,
                isInDisplayedMonth: day.isInDisplayedMonth,
                isRestColored: isRestColored,
                subtitle: isFestival ? (entry?.name ?? "") : (showLunar ? Lunar.text(for: day.date) : ""),
                isFestival: isFestival,
                isTerm: !isFestival && showLunar && term != nil,
                badge: entry?.kind,
                hasEvent: eventDays.contains(day.date),
            )
        }
    }

    var body: some View {
        VStack(spacing: 8) {
            header
            if picking {
                monthPicker
            } else {
                weekdayRow
                grid
            }
            Divider().overlay(theme.cardBorder)
            detail
            eventsSection
            if let settingsError {
                Text(settingsError)
                    .font(.caption)
                    .foregroundStyle(theme.rest)
            }
            footer
        }
        .padding(12)
        .frame(width: 316)
        .foregroundStyle(theme.textPrimary)
        .background(theme.background)
        .environment(\.theme, theme)
        .preferredColorScheme(pinnedColorScheme)
        .onAppear { rebuildCells() }
        .onChange(of: displayed) { rebuildCells() }
        .onChange(of: weekStartRaw) { rebuildCells() }
        .onChange(of: showLunar) { rebuildCells() }
        .onChange(of: eventStore.revision) { rebuildCells() }
        .onChange(of: holidaySync.revision) { rebuildCells() }
        // The MenuBarExtra window is kept alive between opens, so @State
        // survives dismissal. Reset to today's view whenever the popover
        // loses key status (= it was dismissed), so every reopen starts
        // at the current month with today selected.
        .onReceive(NotificationCenter.default.publisher(for: NSWindow.didResignKeyNotification)) { _ in
            resetToToday()
        }
    }

    private func resetToToday() {
        let today = CalendarModel.calendar.startOfDay(for: Date())
        displayed = YearMonth.containing(today)
        selected = today
        picking = false
        rebuildCells()
    }

    // MARK: - Header

    private var header: some View {
        HStack {
            navButton(systemImage: "chevron.left", help: picking ? "上一年" : "上个月") {
                if picking {
                    pickerYear -= 1
                } else {
                    displayed = displayed.shifted(by: -1)
                }
            }
            Spacer()
            Button {
                pickerYear = displayed.year
                picking.toggle()
            } label: {
                Text(displayed.title)
                    .font(.system(.headline, design: .rounded))
                    .foregroundStyle(picking ? theme.accent : theme.textPrimary)
            }
            .buttonStyle(.plain)
            .help("选择年月")
            Spacer()
            navButton(systemImage: "chevron.right", help: picking ? "下一年" : "下个月") {
                if picking {
                    pickerYear += 1
                } else {
                    displayed = displayed.shifted(by: 1)
                }
            }
        }
    }

    /// Year/month quick picker: 3×4 month grid for `pickerYear`, swapped
    /// in place of the day grid. Height matches the grid so the popover
    /// doesn't jump when toggling.
    private var monthPicker: some View {
        let todayYM = YearMonth.containing(Date())
        return VStack(spacing: 10) {
            LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 6), count: 3), spacing: 6) {
                ForEach(1 ... 12, id: \.self) { month in
                    let isCurrent = pickerYear == displayed.year && month == displayed.month
                    let isTodayMonth = pickerYear == todayYM.year && month == todayYM.month
                    Button {
                        displayed = YearMonth(year: pickerYear, month: month)
                        picking = false
                    } label: {
                        Text("\(month)月")
                            .font(.system(.callout, design: .rounded))
                            .fontWeight(isTodayMonth ? .semibold : .regular)
                            .foregroundStyle(isCurrent ? theme.background : theme.textPrimary)
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, 9)
                            .background {
                                RoundedRectangle(cornerRadius: 8)
                                    .fill(isCurrent ? theme.accent : theme.cardBackground)
                            }
                    }
                    .buttonStyle(.plain)
                }
            }
            Button("回到今天") {
                displayed = todayYM
                selected = CalendarModel.calendar.startOfDay(for: Date())
                picking = false
            }
            .font(.callout)
            .buttonStyle(.plain)
            .foregroundStyle(theme.accent)
        }
        .frame(height: 266)
    }

    private func navButton(systemImage: String, help: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: systemImage)
                .font(.callout.weight(.semibold))
                .foregroundStyle(theme.textSecondary)
                .frame(width: 22, height: 22)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help(help)
    }

    // MARK: - Grid

    private var weekdayRow: some View {
        let symbols = CalendarModel.weekdaySymbols(weekStart: weekStart)
        return LazyVGrid(columns: gridColumns, spacing: 0) {
            ForEach(Array(symbols.enumerated()), id: \.offset) { index, symbol in
                Text(symbol)
                    .font(.system(size: 10, weight: .medium))
                    .foregroundStyle(isWeekendColumn(index) ? theme.rest.opacity(0.8) : theme.textSecondary)
                    .frame(maxWidth: .infinity)
            }
        }
    }

    private func isWeekendColumn(_ index: Int) -> Bool {
        let weekday = (weekStart.rawValue - 1 + index) % 7 + 1
        return weekday == 1 || weekday == 7
    }

    private let gridColumns = Array(repeating: GridItem(.flexible(), spacing: 0), count: 7)

    private var grid: some View {
        LazyVGrid(columns: gridColumns, spacing: 0) {
            ForEach(cells) { data in
                DayCell(data: data, isSelected: selected == data.date) {
                    selected = data.date
                }
                .equatable()
            }
        }
    }

    // MARK: - Detail

    private var detail: some View {
        let cal = CalendarModel.calendar
        let month = cal.component(.month, from: selected)
        let day = cal.component(.day, from: selected)
        var text = Text("\(month)月\(day)日 \(CalendarModel.weekdayName(selected))")
            .foregroundStyle(theme.textPrimary)
        if showLunar {
            text = text + Text(" · \(Lunar.longText(for: selected))")
                .foregroundStyle(theme.textSecondary)
        }
        if let entry = Holidays.entry(for: selected) {
            let colored = entry.kind == .rest ? theme.rest : theme.warning
            text = text + Text(" · \(entry.name)\(entry.kind == .work ? "补班" : "")")
                .foregroundStyle(colored)
        }
        return text
            .font(.callout)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, 2)
    }

    // MARK: - Events

    /// The selected day's system-calendar events, or a compact permission
    /// prompt when access hasn't been granted yet. Granted + no events
    /// renders nothing, keeping the popover compact.
    @ViewBuilder
    private var eventsSection: some View {
        switch eventStore.status {
        case .notDetermined:
            permissionRow(text: "允许访问系统日历以显示日程", buttonTitle: "允许") {
                eventStore.requestAccess()
            }
        case .denied:
            permissionRow(text: "日历访问未开启，无法显示日程", buttonTitle: "去设置") {
                if let url = URL(
                    string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Calendars",
                ) {
                    NSWorkspace.shared.open(url)
                }
            }
        case .fullAccess:
            let dayEvents = eventStore.events(on: selected)
            if !dayEvents.isEmpty {
                VStack(alignment: .leading, spacing: 5) {
                    ForEach(Array(dayEvents.prefix(4).enumerated()), id: \.offset) { _, event in
                        eventRow(event)
                    }
                    if dayEvents.count > 4 {
                        Text("还有 \(dayEvents.count - 4) 个日程…")
                            .font(.caption2)
                            .foregroundStyle(theme.textSecondary)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.horizontal, 2)
            }
        }
    }

    private func permissionRow(
        text: String,
        buttonTitle: String,
        action: @escaping () -> Void,
    ) -> some View {
        HStack(spacing: 6) {
            Image(systemName: "calendar.badge.exclamationmark")
                .font(.caption)
                .foregroundStyle(theme.warning)
            Text(text)
                .font(.caption)
                .foregroundStyle(theme.textSecondary)
            Spacer()
            Button(buttonTitle, action: action)
                .font(.caption)
                .buttonStyle(.plain)
                .foregroundStyle(theme.accent)
        }
        .padding(.horizontal, 2)
    }

    /// One event: calendar color bar, time range, title.
    private func eventRow(_ event: EKEvent) -> some View {
        HStack(spacing: 6) {
            RoundedRectangle(cornerRadius: 1.5)
                .fill(Color(nsColor: event.calendar.color))
                .frame(width: 3, height: 12)
            Text(timeLabel(event))
                .font(.caption.monospacedDigit())
                .foregroundStyle(theme.textSecondary)
                .frame(width: 62, alignment: .leading)
            Text(event.title ?? "")
                .font(.caption)
                .lineLimit(1)
            Spacer(minLength: 0)
        }
    }

    private func timeLabel(_ event: EKEvent) -> String {
        if event.isAllDay {
            return "全天"
        }
        let cal = CalendarModel.calendar
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm"
        var label = formatter.string(from: event.startDate)
        // Append the end time only for same-day events.
        if cal.startOfDay(for: event.endDate) == cal.startOfDay(for: event.startDate) {
            label += "-" + formatter.string(from: event.endDate)
        }
        return label
    }

    // MARK: - Footer

    private var footer: some View {
        HStack {
            Spacer()
            settingsMenu
        }
        .padding(.horizontal, 2)
    }

    private var settingsMenu: some View {
        Menu {
            Picker("主题", selection: $themePreference) {
                ForEach(ThemePreference.allCases, id: \.rawValue) { preference in
                    Text(preference.label).tag(preference.rawValue)
                }
            }
            Picker("菜单栏格式", selection: clockFormat) {
                ForEach(ClockFormat.allCases, id: \.rawValue) { format in
                    Text(format.label).tag(format.rawValue)
                }
            }
            Picker("每周第一天", selection: $weekStartRaw) {
                ForEach(WeekStart.allCases, id: \.rawValue) { start in
                    Text(start.label).tag(start.rawValue)
                }
            }
            Toggle("显示农历", isOn: $showLunar)
            Divider()
            ModuleToggles(current: "calendar")
            Toggle("开机自启", isOn: LaunchAtLogin.binding { settingsError = $0 })
            Divider()
            Button("退出 AuraBar", role: .destructive, action: quitApp)
        } label: {
            Image(nsImage: TintedSymbol.make("gearshape", color: theme.textSecondary))
                .frame(width: 18, height: 14)
                .contentShape(Rectangle())
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.hidden)
    }

    private var clockFormat: Binding<String> {
        Binding(
            get: { clock.format.rawValue },
            set: { clock.format = ClockFormat(rawValue: $0) ?? .full },
        )
    }
}
