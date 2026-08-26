import Charts
import SwiftUI

extension AirQuality {
    /// US EPA AQI category, 优 → 严重.
    var category: String {
        switch aqi {
        case ...50: "优"
        case ...100: "良"
        case ...150: "轻度"
        case ...200: "中度"
        case ...300: "重度"
        default: "严重"
        }
    }

    /// Semantic color per Everforest: green ok, yellow moderate, orange
    /// light pollution, red beyond.
    func color(_ theme: Theme) -> Color {
        switch aqi {
        case ...50: theme.ok
        case ...100: theme.warning
        case ...150: theme.orange
        default: theme.rest
        }
    }
}

extension WeatherCondition {
    /// Semantic color per Everforest: warm yellow sun, teal rain, aqua
    /// snow, muted grays for gloom, red thunder.
    func color(_ theme: Theme) -> Color {
        switch self {
        case .clear: theme.warning
        case .partlyCloudy: theme.orange
        case .overcast, .fog, .haze: theme.textSecondary
        case .drizzle, .rain, .heavyRain: theme.accent
        case .sleet, .snow, .hail: theme.aqua
        case .thunder: theme.rest
        case .unknown: theme.textSecondary
        }
    }
}

/// The popover shown when the weather menu bar item is clicked: a
/// current-conditions card, a 24h temperature curve, a 7-day forecast
/// list, a collapsible settings section and a compact footer.
struct WeatherPopover: View {
    @ObservedObject var store: WeatherStore

    @AppStorage("themePreference") private var themePreference = ThemePreference.system.rawValue
    @Environment(\.colorScheme) private var colorScheme

    @State private var settingsExpanded = false
    @State private var settingsError: String?
    /// Drafts applied to the store only on submit, so typing doesn't
    /// trigger a geocoding request per keystroke.
    @State private var cityDraft = ""
    @State private var keyDraft = ""

    private var theme: Theme {
        (ThemePreference(rawValue: themePreference) ?? .system).theme(for: colorScheme)
    }

    private var pinnedColorScheme: ColorScheme? {
        (ThemePreference(rawValue: themePreference) ?? .system).pinnedColorScheme
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            if let snapshot = store.snapshot {
                currentCard(snapshot)
                hourlyCard(snapshot)
                dailyCard(snapshot)
            } else {
                placeholder
            }
            if let error = store.lastError {
                Text(error)
                    .font(.caption)
                    .foregroundStyle(theme.rest)
            }
            settingsSection
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
        .animation(.easeInOut(duration: 0.25), value: store.snapshot != nil)
        .onAppear {
            cityDraft = store.location?.name ?? ""
            keyDraft = store.qweatherKey
        }
    }

    // MARK: - Current conditions

    private func currentCard(_ snapshot: WeatherSnapshot) -> some View {
        let current = snapshot.current
        return VStack(alignment: .leading, spacing: 6) {
            HStack(alignment: .top) {
                VStack(alignment: .leading, spacing: 2) {
                    locationMenu(snapshot)
                    Text("\(current.condition.label) · 体感 \(Int(current.apparentTemperature.rounded()))°")
                        .font(.caption)
                        .foregroundStyle(theme.textSecondary)
                }
                Spacer()
                Image(systemName: current.condition.symbolName)
                    .font(.system(size: 28))
                    .foregroundStyle(current.condition.color(theme))
            }
            Text("\(Int(current.temperature.rounded()))°")
                .font(.system(size: 42, weight: .light, design: .rounded))
                .contentTransition(.numericText())
            HStack(spacing: 10) {
                Label("湿度 \(Int(current.humidity.rounded()))%", systemImage: "humidity")
                Label("\(Int(current.windSpeedKmh.rounded())) km/h", systemImage: "wind")
                if let today = snapshot.daily.first {
                    Label(
                        "\(Int(today.tempMin.rounded()))~\(Int(today.tempMax.rounded()))°",
                        systemImage: "thermometer.medium",
                    )
                }
            }
            .font(.caption2)
            .foregroundStyle(theme.textSecondary)
            let today = snapshot.daily.first
            if today?.sunrise != nil || today?.sunset != nil || snapshot.airQuality != nil {
                HStack(spacing: 10) {
                    if let sunrise = today?.sunrise {
                        Label("日出 \(Self.clockTime(sunrise))", systemImage: "sunrise")
                    }
                    if let sunset = today?.sunset {
                        Label("日落 \(Self.clockTime(sunset))", systemImage: "sunset")
                    }
                    if let aq = snapshot.airQuality {
                        Spacer()
                        HStack(spacing: 4) {
                            Circle()
                                .fill(aq.color(theme))
                                .frame(width: 6, height: 6)
                            Text("AQI \(aq.aqi) · \(aq.category)")
                        }
                    }
                }
                .font(.caption2)
                .foregroundStyle(theme.textSecondary)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .cardStyle()
    }

    private static let clockFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm"
        return formatter
    }()

    private static func clockTime(_ date: Date) -> String {
        clockFormatter.string(from: date)
    }

    /// Location name with a quick-switch dropdown: auto-location plus
    /// every saved city.
    private func locationMenu(_ snapshot: WeatherSnapshot) -> some View {
        Menu {
            Button {
                store.setAutoLocation(true)
            } label: {
                HStack {
                    Label("自动定位", systemImage: "location")
                    if store.autoLocation {
                        Image(systemName: "checkmark")
                    }
                }
            }
            if !store.savedLocations.isEmpty {
                Divider()
                ForEach(store.savedLocations) { loc in
                    Button {
                        store.selectLocation(loc)
                    } label: {
                        HStack {
                            Text(loc.name)
                            if loc == snapshot.location, !store.autoLocation {
                                Image(systemName: "checkmark")
                            }
                        }
                    }
                }
            }
        } label: {
            HStack(spacing: 3) {
                Text(snapshot.location.name)
                    .font(.system(.headline, design: .rounded))
                Image(systemName: "chevron.up.chevron.down")
                    .font(.system(size: 8, weight: .semibold))
                    .foregroundStyle(theme.textSecondary)
            }
            .contentShape(Rectangle())
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.hidden)
    }

    // MARK: - 24h curve

    private func hourlyCard(_ snapshot: WeatherSnapshot) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("未来 24 小时")
                .font(.caption)
                .foregroundStyle(theme.textSecondary)
            Chart {
                ForEach(snapshot.hourly) { point in
                    AreaMark(
                        x: .value("时间", point.date),
                        y: .value("温度", point.temperature),
                    )
                    .interpolationMethod(.catmullRom)
                    .foregroundStyle(
                        LinearGradient(
                            colors: [theme.accent.opacity(0.30), theme.accent.opacity(0.02)],
                            startPoint: .top,
                            endPoint: .bottom,
                        ),
                    )
                    LineMark(
                        x: .value("时间", point.date),
                        y: .value("温度", point.temperature),
                    )
                    .interpolationMethod(.catmullRom)
                    .foregroundStyle(theme.accent)
                    .lineStyle(StrokeStyle(lineWidth: 1.6))
                }
                if let top = snapshot.hourly.max(by: { $0.temperature < $1.temperature }) {
                    PointMark(
                        x: .value("时间", top.date),
                        y: .value("温度", top.temperature),
                    )
                    .foregroundStyle(theme.rest)
                    .symbolSize(18)
                    .annotation(position: .top, spacing: 0) {
                        Text("\(Int(top.temperature.rounded()))°")
                            .font(.system(size: 8))
                            .foregroundStyle(theme.textSecondary)
                    }
                }
                if let bottom = snapshot.hourly.min(by: { $0.temperature < $1.temperature }) {
                    PointMark(
                        x: .value("时间", bottom.date),
                        y: .value("温度", bottom.temperature),
                    )
                    .foregroundStyle(theme.aqua)
                    .symbolSize(18)
                    .annotation(position: .bottom, spacing: 0) {
                        Text("\(Int(bottom.temperature.rounded()))°")
                            .font(.system(size: 8))
                            .foregroundStyle(theme.textSecondary)
                    }
                }
            }
            .chartYAxis(.hidden)
            .chartXAxis {
                AxisMarks(values: .stride(by: .hour, count: 6)) { value in
                    AxisValueLabel {
                        if let date = value.as(Date.self) {
                            Text("\(CalendarModel.calendar.component(.hour, from: date))时")
                                .font(.system(size: 9))
                                .foregroundStyle(theme.textSecondary)
                        }
                    }
                }
            }
            .frame(height: 72)
        }
        .cardStyle()
    }

    // MARK: - 7-day outlook

    private func dailyCard(_ snapshot: WeatherSnapshot) -> some View {
        let weekMin = snapshot.daily.map(\.tempMin).min() ?? 0
        let weekMax = snapshot.daily.map(\.tempMax).max() ?? 1
        return VStack(alignment: .leading, spacing: 7) {
            Text("未来 7 天")
                .font(.caption)
                .foregroundStyle(theme.textSecondary)
            ForEach(snapshot.daily) { day in
                HStack(spacing: 8) {
                    Text(weekdayLabel(day.date))
                        .frame(width: 34, alignment: .leading)
                    Image(systemName: day.condition.symbolName)
                        .foregroundStyle(day.condition.color(theme))
                        .frame(width: 18)
                    HStack(spacing: 1) {
                        Image(systemName: "drop.fill")
                            .font(.system(size: 7))
                        Text("\(day.precipProbability ?? 0)%")
                    }
                    .font(.system(size: 9))
                    .foregroundStyle(theme.accent)
                    .frame(width: 32, alignment: .leading)
                    .opacity((day.precipProbability ?? 0) >= 30 ? 1 : 0)
                    Spacer()
                    Text("\(Int(day.tempMin.rounded()))°")
                        .foregroundStyle(theme.textSecondary)
                    rangeBar(day, weekMin: weekMin, weekMax: weekMax)
                    Text("\(Int(day.tempMax.rounded()))°")
                }
                .font(.callout)
                .monospacedDigit()
            }
        }
        .cardStyle()
    }

    private func weekdayLabel(_ date: Date) -> String {
        let cal = CalendarModel.calendar
        if cal.isDateInToday(date) {
            return "今天"
        }
        let names = ["周日", "周一", "周二", "周三", "周四", "周五", "周六"]
        return names[cal.component(.weekday, from: date) - 1]
    }

    /// iOS-Weather-style temperature range bar: the day's [min, max]
    /// interval positioned within the week's overall span.
    private func rangeBar(_ day: DayForecast, weekMin: Double, weekMax: Double) -> some View {
        let span = max(weekMax - weekMin, 1)
        let start = (day.tempMin - weekMin) / span
        let length = max((day.tempMax - day.tempMin) / span, 0.06)
        return GeometryReader { geo in
            ZStack(alignment: .leading) {
                Capsule().fill(theme.background)
                Capsule()
                    .fill(
                        LinearGradient(
                            colors: [theme.aqua, theme.orange],
                            startPoint: .leading,
                            endPoint: .trailing,
                        ),
                    )
                    .frame(width: geo.size.width * length)
                    .offset(x: geo.size.width * start)
            }
        }
        .frame(width: 72, height: 4)
    }

    // MARK: - Placeholder

    private var placeholder: some View {
        HStack(spacing: 8) {
            if store.isLoading {
                ProgressView().controlSize(.small)
                Text("正在获取天气…")
            } else {
                Image(systemName: "exclamationmark.triangle")
                Text(store.lastError ?? "暂无数据")
            }
        }
        .font(.callout)
        .foregroundStyle(theme.textSecondary)
        .frame(maxWidth: .infinity, alignment: .center)
        .padding(.vertical, 28)
    }

    // MARK: - Settings

    private var settingsSection: some View {
        VStack(alignment: .leading, spacing: 6) {
            CollapsibleHeader(title: "天气设置", expanded: $settingsExpanded)
            if settingsExpanded {
                VStack(alignment: .leading, spacing: 6) {
                    Picker("数据源", selection: $store.providerKind) {
                        ForEach(WeatherProviderKind.allCases, id: \.rawValue) { kind in
                            Text(kind.label).tag(kind)
                        }
                    }
                    .labelsHidden()
                    .pickerStyle(.segmented)

                    Toggle("自动定位", isOn: autoLocationBinding)
                        .font(.caption)
                        .tint(theme.accent)

                    if store.autoLocation {
                        autoLocationStatus
                    } else {
                        SettingsField(prompt: "添加城市（如 北京 / 上海）", text: $cityDraft) {
                            Task { await store.setCity(cityDraft) }
                        }
                        if !store.savedLocations.isEmpty {
                            savedLocationList
                        }
                    }

                    if store.providerKind == .qweather {
                        SettingsField(prompt: "和风天气 API Key", text: $keyDraft) {
                            store.qweatherKey = keyDraft
                            Task { await store.refresh() }
                        }
                    }
                }
                .padding(.top, 4)
                .transition(.opacity.combined(with: .move(edge: .top)))
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, 2)
    }

    private var autoLocationBinding: Binding<Bool> {
        Binding(
            get: { store.autoLocation },
            set: { store.setAutoLocation($0) },
        )
    }

    /// Status line for auto mode: locating spinner, permission hint with
    /// a shortcut to System Settings, or the resolved place name.
    private var autoLocationStatus: some View {
        HStack(spacing: 6) {
            if store.locationService.isLocating {
                ProgressView().controlSize(.small)
                Text("定位中…")
            } else if store.locationService.authorizationDenied {
                Image(systemName: "location.slash")
                    .foregroundStyle(theme.warning)
                Text("定位未授权")
                Spacer()
                Button("去设置") {
                    if let url = URL(
                        string: "x-apple.systempreferences:com.apple.preference.security?Privacy_LocationServices",
                    ) {
                        NSWorkspace.shared.open(url)
                    }
                }
                .buttonStyle(.plain)
                .foregroundStyle(theme.accent)
            } else {
                Image(systemName: "location")
                Text(store.location.map { "当前：\($0.name)" } ?? "待定位")
            }
        }
        .font(.caption)
        .foregroundStyle(theme.textSecondary)
    }

    /// Saved cities: click to select, × to remove.
    private var savedLocationList: some View {
        VStack(alignment: .leading, spacing: 5) {
            ForEach(store.savedLocations) { loc in
                HStack(spacing: 6) {
                    Button {
                        store.selectLocation(loc)
                    } label: {
                        HStack(spacing: 4) {
                            Image(systemName: "checkmark")
                                .font(.system(size: 8, weight: .bold))
                                .foregroundStyle(theme.accent)
                                .opacity(loc == store.location && !store.autoLocation ? 1 : 0)
                            Text(loc.name)
                                .foregroundStyle(theme.textPrimary)
                        }
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    Spacer()
                    Button {
                        store.removeLocation(loc)
                    } label: {
                        Image(systemName: "xmark")
                            .font(.system(size: 8))
                            .foregroundStyle(theme.textSecondary)
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                }
                .font(.caption)
            }
        }
    }

    // MARK: - Footer

    private var footer: some View {
        HStack(spacing: 10) {
            Text(store.lastUpdated.map { "更新于 \($0.formatted(date: .omitted, time: .shortened))" } ?? "未更新")
                .font(.caption)
                .foregroundStyle(theme.textSecondary)
                .contentTransition(.numericText())
            Spacer()
            RefreshButton(isLoading: store.isLoading, justRefreshed: store.justRefreshed) {
                Task { await store.refresh() }
            }
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
            Divider()
            ModuleToggles(current: "weather")
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
}
