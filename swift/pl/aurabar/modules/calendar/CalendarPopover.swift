import AppKit
import EventKit
import SwiftUI

/// Autosave name of the status item hosting this popover; the close
/// signal (`Notification.Name.statusItemPopoverDidClose`) is filtered
/// on it so only this popover's dismissal — not another module's —
/// triggers a reset.
let calendarModuleAutosaveName = "AuraBar.calendar"

/// The popover shown when the calendar menu bar item is clicked: a month
/// grid with lunar / holiday annotations, the selected day's system
/// calendar events, and a compact footer with a settings menu.
struct CalendarPopover: View {
    /// Plain reference, not @ObservedObject: the popover rebuilds on
    /// `clock.dayChanged` (once per midnight rollover) instead of
    /// re-rendering with every label tick (every second in the
    /// withSeconds format).
    let clock: MenuBarClock
    @ObservedObject var eventStore: EventStore
    @ObservedObject var holidaySync: HolidaySync

    @AppStorage("themePreference") private var themePreference = ThemePreference.system.rawValue
    @AppStorage(ThemeKind.key) private var themeKind = ThemeKind.everforest.rawValue
    // Subscribed (not read) so an accent change re-renders the popover.
    @AppStorage(AccentColor.key) private var accentHex = ""
    @AppStorage("AuraBar.calendar.weekStart") private var weekStartRaw = WeekStart.monday.rawValue
    @AppStorage("AuraBar.calendar.showLunar") private var showLunar = true
    @Environment(\.colorScheme) private var colorScheme

    @State private var displayed = YearMonth.containing(Date())
    @State private var selected = CalendarModel.calendar.startOfDay(for: Date())
    /// Precomputed cell view models; rebuilt off the main thread only
    /// when an input changes (month, week start, lunar toggle, event or
    /// holiday revision, midnight) — never on routine re-renders like
    /// selection changes.
    @State private var cells: [DayCellData] = []
    /// Year/month quick picker state; replaces the grid while active.
    @State private var picking = false
    @State private var pickerYear = YearMonth.containing(Date()).year
    /// In-flight grid rebuild; superseded rebuilds are cancelled so a
    /// slow stale build (EventKit daemon round-trip) never overwrites
    /// a newer grid.
    @State private var rebuildTask: Task<Void, Never>?
    /// Events listed for the selected day; loaded asynchronously.
    @State private var selectedDayEvents: [EKEvent] = []
    @State private var eventsTask: Task<Void, Never>?
    /// Arrow-key grid navigation needs the container focused.
    @FocusState private var gridFocused: Bool

    private var theme: Theme {
        (ThemePreference(rawValue: themePreference) ?? .system).theme(
            for: colorScheme,
            kind: ThemeKind(rawValue: themeKind) ?? .everforest,
        )
    }

    private var pinnedColorScheme: ColorScheme? {
        (ThemePreference(rawValue: themePreference) ?? .system).pinnedColorScheme
    }

    private var weekStart: WeekStart {
        WeekStart(rawValue: weekStartRaw) ?? .monday
    }

    /// Rebuild the precomputed cell models from all inputs off the main
    /// thread: 42 days of lunar conversion + solar terms + holiday
    /// lookups, plus one EventKit range query for the dot markers.
    /// Lunar text is the expensive part, and the store query talks to
    /// the calendar daemon — neither belongs on the main thread.
    private func rebuildCells() {
        rebuildTask?.cancel()
        let month = displayed
        let weekStart = weekStart
        let showLunar = showLunar
        let store = eventStore
        // Capture the @State binding explicitly so the detached task
        // can write the result back on the main actor.
        rebuildTask = Task.detached { [binding = $cells] in
            let built = buildCalendarCells(
                month: month,
                weekStart: weekStart,
                showLunar: showLunar,
                eventStore: store,
            )
            guard !Task.isCancelled else { return }
            await MainActor.run {
                binding.wrappedValue = built
            }
        }
    }

    /// Load the selected day's events off the main thread (the query
    /// round-trips to the calendar daemon).
    private func reloadSelectedDayEvents() {
        eventsTask?.cancel()
        let day = selected
        let store = eventStore
        eventsTask = Task.detached { [binding = $selectedDayEvents] in
            let events = store.events(on: day)
            guard !Task.isCancelled else { return }
            await MainActor.run {
                binding.wrappedValue = events
            }
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
            holidayCountdown
            eventsSection
            footer
        }
        .padding(12)
        .frame(width: 316)
        .foregroundStyle(theme.textPrimary)
        .background(theme.background)
        .environment(\.theme, theme)
        .preferredColorScheme(pinnedColorScheme)
        // Arrow-key day navigation handled at the container; the focus
        // ring itself is suppressed (selection highlight is enough).
        .focusable()
        .focused($gridFocused)
        .focusEffectDisabled()
        .onKeyPress { press in
            guard !picking else { return .ignored }
            switch press.key {
            case .leftArrow: moveSelection(by: -1)
            case .rightArrow: moveSelection(by: 1)
            case .upArrow: moveSelection(by: -7)
            case .downArrow: moveSelection(by: 7)
            default: return .ignored
            }
            return .handled
        }
        .onAppear {
            gridFocused = true
            rebuildCells()
            reloadSelectedDayEvents()
        }
        .onChange(of: displayed) { _, _ in rebuildCells() }
        .onChange(of: weekStartRaw) { _, _ in rebuildCells() }
        .onChange(of: showLunar) { _, _ in rebuildCells() }
        .onChange(of: eventStore.revision) { _, _ in
            rebuildCells()
            reloadSelectedDayEvents()
        }
        .onChange(of: eventStore.status) { _, _ in reloadSelectedDayEvents() }
        .onChange(of: holidaySync.revision) { _, _ in rebuildCells() }
        .onChange(of: selected) { _, _ in reloadSelectedDayEvents() }
        // Midnight rollover, fired exactly once per local-day change.
        .onReceive(clock.dayChanged) { _ in rebuildCells() }
        // The popover window is kept alive between opens, so @State
        // survives dismissal. Reset to today's view whenever this
        // popover (identified by its status item's autosave name) is
        // dismissed, so every reopen starts at the current month with
        // today selected. NSWindow.didResignKeyNotification would fire
        // for *any* window — e.g. opening settings from the gear menu —
        // and reset the calendar while it's still visible.
        .onReceive(NotificationCenter.default.publisher(for: .statusItemPopoverDidClose)) { note in
            guard note.userInfo?[Notification.Name.statusItemAutosaveNameKey] as? String
                == calendarModuleAutosaveName else { return }
            resetToToday()
        }
        // Time zone changes shift day boundaries; rebuild for the new
        // local "today".
        .onReceive(NotificationCenter.default.publisher(
            for: NSNotification.Name.NSSystemTimeZoneDidChange,
        )) { _ in
            resetToToday()
        }
    }

    private func resetToToday() {
        let today = CalendarModel.calendar.startOfDay(for: Date())
        displayed = YearMonth.containing(today)
        selected = today
        picking = false
        rebuildCells()
        reloadSelectedDayEvents()
    }

    /// Select a day; when it lies outside the displayed month (a dimmed
    /// leading/trailing cell), jump the grid to its month as well.
    private func select(date: Date) {
        let day = CalendarModel.calendar.startOfDay(for: date)
        selected = day
        let month = YearMonth.containing(day)
        if month != displayed {
            displayed = month
        }
    }

    /// Arrow-key navigation: ±1 day (left/right) or ±1 week (up/down),
    /// crossing months via `select`.
    private func moveSelection(by days: Int) {
        guard let next = CalendarModel.calendar.date(byAdding: .day, value: days, to: selected)
        else { return }
        select(date: next)
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
            Button("回到今天", action: resetToToday)
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
                    select(date: data.date)
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

    /// "距 国庆节 还有 23 天"（或假期中的第 N 天）——法节假表已有数据
    /// 的最强心智信息。
    private var holidayCountdown: some View {
        guard let block = Holidays.nextHoliday(from: Date()) else {
            return AnyView(EmptyView())
        }
        let cal = CalendarModel.calendar
        let today = cal.startOfDay(for: Date())
        let delta = cal.dateComponents([.day], from: today, to: block.start).day ?? 0
        var text: Text
        if delta > 0 {
            text = Text("距 ").foregroundStyle(theme.textSecondary)
                + Text(block.name).foregroundStyle(theme.rest)
                + Text(" 还有 \(delta) 天").foregroundStyle(theme.textPrimary)
                + Text(" · \(formatDay(block.start))-\(formatDay(block.end)) 共 \(block.days) 天")
                .foregroundStyle(theme.textSecondary)
        } else {
            let nth = (cal.dateComponents([.day], from: block.start, to: today).day ?? 0) + 1
            text = Text(block.name).foregroundStyle(theme.rest)
                + Text(" 假期中 · 第 \(nth)/\(block.days) 天").foregroundStyle(theme.textPrimary)
        }
        return AnyView(
            text
                .font(.caption)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.horizontal, 2),
        )
    }

    private func formatDay(_ date: Date) -> String {
        let cal = CalendarModel.calendar
        return "\(cal.component(.month, from: date)).\(cal.component(.day, from: date))"
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
                    NotificationCenter.default.post(name: .statusItemPopoverCloseRequest, object: nil)
                }
            }
        case .fullAccess:
            if !selectedDayEvents.isEmpty {
                VStack(alignment: .leading, spacing: 3) {
                    ForEach(Array(selectedDayEvents.prefix(4).enumerated()), id: \.offset) { _, event in
                        EventRow(event: event) {
                            Self.openCalendarApp()
                            NotificationCenter.default.post(name: .statusItemPopoverCloseRequest, object: nil)
                        }
                    }
                    if selectedDayEvents.count > 4 {
                        Text("还有 \(selectedDayEvents.count - 4) 个日程…")
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

    /// Activate the system Calendar app. (Deep-linking to a specific
    /// event needs a calendaritem:// URL whose identifiers don't map
    /// reliably to EventKit's opaque IDs; activation is the robust
    /// affordance.)
    private static func openCalendarApp() {
        guard let url = NSWorkspace.shared.urlForApplication(withBundleIdentifier: "com.apple.iCal")
        else { return }
        NSWorkspace.shared.openApplication(at: url, configuration: NSWorkspace.OpenConfiguration())
    }

    // MARK: - Footer

    private var isViewingToday: Bool {
        let today = CalendarModel.calendar.startOfDay(for: Date())
        return !picking && selected == today && displayed == YearMonth.containing(today)
    }

    private var footer: some View {
        HStack {
            // Always laid out (invisible while viewing today) so the
            // footer height doesn't jump when the button appears.
            Button("回到今天", action: resetToToday)
                .font(.caption)
                .buttonStyle(.plain)
                .foregroundStyle(theme.accent)
                .opacity(isViewingToday ? 0 : 1)
                .disabled(isViewingToday)
            Spacer()
            settingsMenu
        }
        .padding(.horizontal, 2)
    }

    private var settingsMenu: some View {
        Menu {
            Button("设置…") {
                SettingsWindowController.shared.show()
            }
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
}

/// Pure grid construction: 42 cells with lunar / solar-term /
/// festival subtitles plus event dots from a single range query.
/// File-scope (not a CalendarPopover member) so it stays free of the
/// MainActor isolation SwiftUI infers for View types — it's called
/// from detached rebuild tasks.
private func buildCalendarCells(
    month: YearMonth,
    weekStart: WeekStart,
    showLunar: Bool,
    eventStore: EventStore,
) -> [DayCellData] {
    let grid = CalendarModel.monthGrid(month, weekStart: weekStart)
    // Dot markers need one range query covering the whole grid.
    let eventDays: Set<Date> = {
        guard let first = grid.first?.date, let last = grid.last?.date,
              let end = CalendarModel.calendar.date(byAdding: .day, value: 1, to: last)
        else { return [] }
        return eventStore.eventDays(from: first, to: end)
    }()
    return grid.map { day in
        let entry = Holidays.entry(for: day.date)
        let isFestival = entry?.isStatutoryFestival ?? false
        // One lookup per day serves both the subtitle and its color.
        let term = SolarTerms.term(for: day.date)
        let isRestColored = entry?.kind == .work ? false : (entry?.kind == .rest || day.isWeekend)
        return DayCellData(
            date: day.date,
            day: day.day,
            isToday: day.isToday,
            isInDisplayedMonth: day.isInDisplayedMonth,
            isRestColored: isRestColored,
            subtitle: isFestival ? (entry?.name ?? "") : (showLunar ? term ?? Lunar.dayText(for: day.date) : ""),
            isFestival: isFestival,
            isTerm: !isFestival && showLunar && term != nil,
            badge: entry?.kind,
            hasEvent: eventDays.contains(day.date),
        )
    }
}

/// "HH:mm" formatter shared by event rows — DateFormatter creation is
/// expensive, don't make one per row per render.
private let eventTimeFormatter: DateFormatter = {
    let f = DateFormatter()
    f.dateFormat = "HH:mm"
    return f
}()

/// Compact time range for an event row, e.g. "14:00-15:00" (end time
/// appended for same-day events only) or "全天".
private func eventTimeLabel(_ event: EKEvent) -> String {
    if event.isAllDay {
        return "全天"
    }
    let cal = CalendarModel.calendar
    var label = eventTimeFormatter.string(from: event.startDate)
    if cal.startOfDay(for: event.endDate) == cal.startOfDay(for: event.startDate) {
        label += "-" + eventTimeFormatter.string(from: event.endDate)
    }
    return label
}

/// One event row: calendar color bar, time range, title. Clicking runs
/// `action` (open in Calendar); a subtle hover background signals
/// clickability — plain buttons give no feedback on macOS.
private struct EventRow: View {
    let event: EKEvent
    let action: () -> Void

    @Environment(\.theme) private var theme
    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            HStack(spacing: 6) {
                RoundedRectangle(cornerRadius: 1.5)
                    .fill(Color(nsColor: event.calendar.color))
                    .frame(width: 3, height: 12)
                Text(eventTimeLabel(event))
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(theme.textSecondary)
                    .frame(width: 62, alignment: .leading)
                Text(event.title ?? "")
                    .font(.caption)
                    .lineLimit(1)
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 4)
            .padding(.vertical, 1)
            .background {
                RoundedRectangle(cornerRadius: 5)
                    .fill(theme.cardBackground)
                    .opacity(hovering ? 1 : 0)
            }
            // The hover background bleeds 4pt past the text edge; take
            // it back so the column stays aligned with the rows around
            // it.
            .padding(.horizontal, -4)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help("在日历 App 中查看")
        .onHover { hovering = $0 }
        .animation(.easeOut(duration: 0.12), value: hovering)
    }
}
