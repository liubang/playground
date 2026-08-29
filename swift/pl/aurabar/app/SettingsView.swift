import SwiftUI

/// The standalone settings window, styled after macOS System Settings:
/// an icon sidebar on the left, themed section cards with icon-badge
/// rows on the right. Replaces the cramped per-popover gear menus.
struct SettingsView: View {
    enum Tab: String, CaseIterable {
        case general
        case calendar
        case weather
        case about

        var label: String {
            switch self {
            case .general: "通用"
            case .calendar: "日历"
            case .weather: "天气"
            case .about: "关于"
            }
        }

        var icon: String {
            switch self {
            case .general: "gearshape"
            case .calendar: "calendar"
            case .weather: "cloud.sun"
            case .about: "info.circle"
            }
        }
    }

    @State private var selected: Tab = .general

    @AppStorage("themePreference") private var themePreference = ThemePreference.system.rawValue
    // Subscribed (not read) so an accent change re-renders the window.
    @AppStorage(AccentColor.key) private var accentHex = ""
    @Environment(\.colorScheme) private var colorScheme

    private var theme: Theme {
        (ThemePreference(rawValue: themePreference) ?? .system).theme(for: colorScheme)
    }

    private var pinnedColorScheme: ColorScheme? {
        (ThemePreference(rawValue: themePreference) ?? .system).pinnedColorScheme
    }

    private var version: String {
        Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?"
    }

    var body: some View {
        HStack(spacing: 0) {
            sidebar
            Divider().overlay(theme.cardBorder)
            content
        }
        .frame(width: 640, height: 400)
        .background(theme.background)
        .foregroundStyle(theme.textPrimary)
        .environment(\.theme, theme)
        .preferredColorScheme(pinnedColorScheme)
    }

    // MARK: - Sidebar

    private var sidebar: some View {
        VStack(alignment: .leading, spacing: 2) {
            ForEach(Tab.allCases, id: \.self) { tab in
                Button {
                    selected = tab
                } label: {
                    HStack(spacing: 8) {
                        Image(systemName: tab.icon)
                            .font(.callout)
                            .frame(width: 20)
                        Text(tab.label)
                            .font(.callout)
                    }
                    .foregroundStyle(selected == tab ? theme.accent : theme.textPrimary)
                    .padding(.horizontal, 10)
                    .padding(.vertical, 6)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background {
                        if selected == tab {
                            RoundedRectangle(cornerRadius: 6)
                                .fill(theme.accent.opacity(0.15))
                        }
                    }
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
            }
            Spacer()
            Text("AuraBar · v\(version)")
                .font(.caption2)
                .foregroundStyle(theme.textSecondary)
                .padding(.horizontal, 10)
        }
        .padding(10)
        .frame(width: 150)
        .background(theme.cardBackground)
    }

    // MARK: - Content

    private var content: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                Text(selected.label)
                    .font(.system(.title3, design: .rounded).weight(.semibold))
                switch selected {
                case .general: GeneralTab()
                case .calendar: CalendarTab()
                case .weather: WeatherTab()
                case .about: AboutTab()
                }
            }
            .padding(18)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .background(theme.background)
    }
}

// MARK: - Shared components

/// A card grouping related rows, with a small caption above it.
private struct SettingsSection<Content: View>: View {
    let title: String
    @ViewBuilder let content: Content

    @Environment(\.theme) private var theme

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title)
                .font(.caption)
                .foregroundStyle(theme.textSecondary)
                .padding(.horizontal, 4)
            VStack(alignment: .leading, spacing: 0) {
                content
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 4)
            .background(theme.cardBackground, in: RoundedRectangle(cornerRadius: 10))
            .overlay(
                RoundedRectangle(cornerRadius: 10)
                    .stroke(theme.cardBorder, lineWidth: 1),
            )
        }
    }
}

/// Colored rounded-square icon for a settings row, System Settings style.
private struct IconBadge: View {
    let systemName: String
    let color: Color

    var body: some View {
        Image(systemName: systemName)
            .font(.system(size: 11, weight: .semibold))
            .foregroundStyle(.white)
            .frame(width: 22, height: 22)
            .background(color, in: RoundedRectangle(cornerRadius: 5))
    }
}

/// One settings row: icon badge + label on the left, control on the right.
private struct SettingsRow<Control: View>: View {
    let icon: String
    let color: Color
    let label: String
    @ViewBuilder let control: Control

    @Environment(\.theme) private var theme

    var body: some View {
        HStack(spacing: 10) {
            IconBadge(systemName: icon, color: color)
            Text(label)
                .font(.callout)
            Spacer()
            control
        }
        .padding(.vertical, 7)
    }
}

/// Accent-color picker: theme default (empty string), curated presets,
/// then a free ColorPicker well. Persisted as #RRGGBB in UserDefaults;
/// ThemePreference.theme(for:) applies it to every popover.
private struct AccentSwatches: View {
    @AppStorage(AccentColor.key) private var accentHex = ""
    /// The environment theme may already carry the override; the
    /// default swatch needs the palette's pristine accent.
    @Environment(\.colorScheme) private var colorScheme
    @Environment(\.theme) private var theme

    private static let presets = [
        "#7FBBB3", "#0A84FF", "#7D7AFF", "#BF5AF2",
        "#FF6482", "#FF9F0A", "#FFD60A", "#A7C080",
    ]

    /// The theme-default accent for the first swatch.
    private var defaultAccent: Color {
        (colorScheme == .dark ? Theme.dark : Theme.light).accent
    }

    private var isCustom: Bool {
        !accentHex.isEmpty && !Self.presets.contains {
            $0.caseInsensitiveCompare(accentHex) == .orderedSame
        }
    }

    private var customColor: Binding<Color> {
        Binding(
            get: { Color(hexString: accentHex) ?? .gray },
            set: {
                if let hex = Color.hexString(of: $0) {
                    accentHex = hex
                }
            },
        )
    }

    var body: some View {
        HStack(spacing: 7) {
            swatchButton(color: defaultAccent, selected: accentHex.isEmpty) {
                accentHex = ""
            }
            ForEach(Self.presets, id: \.self) { hex in
                swatchButton(
                    color: Color(hexString: hex) ?? .gray,
                    selected: accentHex.caseInsensitiveCompare(hex) == .orderedSame,
                ) {
                    accentHex = hex
                }
            }
            ColorPicker("", selection: customColor, supportsOpacity: false)
                .labelsHidden()
                .frame(width: 20, height: 20)
                .clipShape(Circle())
                .overlay {
                    if isCustom {
                        selectionRing
                            .allowsHitTesting(false)
                    }
                }
                .help("自定义…")
        }
    }

    private func swatchButton(
        color: Color,
        selected: Bool,
        action: @escaping () -> Void,
    ) -> some View {
        Button(action: action) {
            Circle()
                .fill(color)
                .frame(width: 16, height: 16)
                .overlay {
                    if selected { selectionRing }
                }
                .contentShape(Circle())
        }
        .buttonStyle(.plain)
    }

    private var selectionRing: some View {
        Circle()
            .stroke(theme.textPrimary.opacity(0.55), lineWidth: 1.5)
            .padding(-3)
    }
}

/// Divider between rows inside a section card.
private struct RowDivider: View {
    @Environment(\.theme) private var theme

    var body: some View {
        Divider()
            .overlay(theme.cardBorder)
            .padding(.leading, 32)
    }
}

// MARK: - 通用

private struct GeneralTab: View {
    @AppStorage("themePreference") private var themePreference = ThemePreference.system.rawValue
    @AppStorage(ModuleVisibility.calendarKey) private var calendar = true
    @AppStorage(ModuleVisibility.weatherKey) private var weather = true
    @AppStorage(ModuleVisibility.cpuKey) private var cpu = true
    @AppStorage(ModuleVisibility.memoryKey) private var memory = true
    @AppStorage(ModuleVisibility.networkKey) private var network = true
    @AppStorage(ModuleVisibility.batteryKey) private var battery = true

    @Environment(\.theme) private var theme
    @State private var launchError: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            SettingsSection(title: "外观") {
                SettingsRow(icon: "paintpalette", color: theme.accent, label: "主题") {
                    Picker("主题", selection: $themePreference) {
                        ForEach(ThemePreference.allCases, id: \.rawValue) { preference in
                            Text(preference.label).tag(preference.rawValue)
                        }
                    }
                    .labelsHidden()
                    .pickerStyle(.segmented)
                    .frame(width: 200)
                }
                RowDivider()
                SettingsRow(icon: "paintbrush.fill", color: theme.accent, label: "强调色") {
                    AccentSwatches()
                }
            }
            SettingsSection(title: "菜单栏模块") {
                LazyVGrid(
                    columns: Array(repeating: GridItem(.flexible(), alignment: .leading), count: 3),
                    spacing: 8,
                ) {
                    moduleToggle("日历", $calendar, "calendar")
                    moduleToggle("天气", $weather, "weather")
                    moduleToggle("CPU", $cpu, "cpu")
                    moduleToggle("内存", $memory, "memory")
                    moduleToggle("网络", $network, "network")
                    // On machines without an internal battery the module
                    // simply doesn't exist as an option.
                    if AppRegistry.hasBattery {
                        moduleToggle("电池", $battery, "battery")
                    }
                }
                .padding(.vertical, 8)
            }
            SettingsSection(title: "系统") {
                SettingsRow(icon: "power", color: theme.ok, label: "开机自启") {
                    Toggle("", isOn: LaunchAtLogin.binding { launchError = $0 })
                        .labelsHidden()
                        .toggleStyle(.switch)
                        .tint(theme.accent)
                }
                if let launchError {
                    Text(launchError)
                        .font(.caption)
                        .foregroundStyle(theme.rest)
                        .padding(.bottom, 6)
                }
            }
        }
    }

    private func moduleToggle(_ label: String, _ binding: Binding<Bool>, _ key: String) -> some View {
        Toggle(label, isOn: binding)
            .font(.callout)
            .toggleStyle(.switch)
            .tint(theme.accent)
            .disabled(othersAllOff(key))
    }

    /// At least one module must stay visible, otherwise there'd be no
    /// status item left to reopen settings from.
    private func othersAllOff(_ except: String) -> Bool {
        let all: [String: Bool] = [
            "calendar": calendar,
            "weather": weather,
            "cpu": cpu,
            "memory": memory,
            "network": network,
            "battery": battery,
        ]
        return all.filter { $0.key != except }.allSatisfy { !$0.value }
    }
}

// MARK: - 日历

private struct CalendarTab: View {
    @AppStorage("AuraBar.calendar.weekStart") private var weekStartRaw = WeekStart.monday.rawValue
    @AppStorage("AuraBar.calendar.showLunar") private var showLunar = true

    @Environment(\.theme) private var theme

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            SettingsSection(title: "菜单栏时钟") {
                SettingsRow(icon: "clock", color: theme.accent, label: "时钟格式") {
                    Picker("时钟格式", selection: clockFormat) {
                        ForEach(ClockFormat.allCases, id: \.rawValue) { format in
                            Text(format.label).tag(format.rawValue)
                        }
                    }
                    .labelsHidden()
                }
                RowDivider()
                SettingsRow(icon: "globe", color: theme.aqua, label: "第二时区") {
                    Picker("第二时区", selection: secondTimeZone) {
                        ForEach(SecondTimeZone.allCases, id: \.rawValue) { zone in
                            Text(zone.label).tag(zone.rawValue)
                        }
                    }
                    .labelsHidden()
                }
            }
            SettingsSection(title: "日历") {
                SettingsRow(icon: "calendar", color: theme.orange, label: "每周第一天") {
                    Picker("每周第一天", selection: $weekStartRaw) {
                        ForEach(WeekStart.allCases, id: \.rawValue) { start in
                            Text(start.label).tag(start.rawValue)
                        }
                    }
                    .labelsHidden()
                }
                RowDivider()
                SettingsRow(icon: "moon.stars", color: theme.accent, label: "显示农历与节气") {
                    Toggle("", isOn: $showLunar)
                        .labelsHidden()
                        .toggleStyle(.switch)
                        .tint(theme.accent)
                }
            }
        }
    }

    private var clockFormat: Binding<String> {
        Binding(
            get: { AppRegistry.clock?.format.rawValue ?? ClockFormat.full.rawValue },
            set: { AppRegistry.clock?.format = ClockFormat(rawValue: $0) ?? .full },
        )
    }

    private var secondTimeZone: Binding<String> {
        Binding(
            get: { AppRegistry.clock?.secondTimeZone.rawValue ?? "" },
            set: { AppRegistry.clock?.secondTimeZone = SecondTimeZone(rawValue: $0) ?? .off },
        )
    }
}

// MARK: - 天气

private struct WeatherTab: View {
    @Environment(\.theme) private var theme
    @State private var cityDraft = ""
    @State private var keyDraft = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            if let store = AppRegistry.weather {
                SettingsSection(title: "数据源") {
                    SettingsRow(icon: "cloud.sun", color: theme.warning, label: "数据源") {
                        Picker("数据源", selection: Binding(
                            get: { store.providerKind },
                            set: { store.providerKind = $0 },
                        )) {
                            ForEach(WeatherProviderKind.allCases, id: \.rawValue) { kind in
                                Text(kind.label).tag(kind)
                            }
                        }
                        .labelsHidden()
                        .pickerStyle(.segmented)
                        .frame(width: 280)
                    }
                    if store.providerKind == .apple {
                        Text("需 Apple Developer 账号为 App ID 开启 WeatherKit capability 并用开发者证书签名后生效。")
                            .font(.caption)
                            .foregroundStyle(theme.textSecondary)
                            .padding(.leading, 32)
                            .padding(.vertical, 7)
                    }
                    if store.providerKind == .qweather {
                        RowDivider()
                        SettingsRow(icon: "key", color: theme.orange, label: "和风 Key") {
                            SettingsField(prompt: "API Key", text: $keyDraft) {
                                store.qweatherKey = keyDraft
                                Task { await store.refresh() }
                            }
                            .frame(width: 200)
                        }
                    }
                }
                SettingsSection(title: "位置") {
                    SettingsRow(icon: "location", color: theme.accent, label: "自动定位") {
                        Toggle("", isOn: Binding(
                            get: { store.autoLocation },
                            set: { store.setAutoLocation($0) },
                        ))
                        .labelsHidden()
                        .toggleStyle(.switch)
                        .tint(theme.accent)
                    }
                    if store.autoLocation {
                        RowDivider()
                        HStack(spacing: 6) {
                            Image(systemName: "location")
                            Text(store.location.map { "当前：\($0.name)" } ?? "待定位")
                            if let source = store.locationService.source {
                                Text(source == .coreLocation ? "· 系统定位" : "· IP 粗定位")
                                    .foregroundStyle(
                                        source == .coreLocation ? theme.textSecondary : theme.warning,
                                    )
                            }
                        }
                        .font(.caption)
                        .foregroundStyle(theme.textSecondary)
                        .padding(.leading, 32)
                        .padding(.vertical, 7)
                    } else {
                        RowDivider()
                        SettingsRow(icon: "plus.circle", color: theme.ok, label: "添加城市") {
                            SettingsField(prompt: "如 北京 / 上海", text: $cityDraft) {
                                Task {
                                    await store.setCity(cityDraft)
                                    cityDraft = ""
                                }
                            }
                            .frame(width: 160)
                        }
                        if !store.savedLocations.isEmpty {
                            RowDivider()
                            savedLocationList(store)
                                .padding(.leading, 32)
                                .padding(.vertical, 7)
                        }
                    }
                }
            }
        }
        .onAppear {
            keyDraft = AppRegistry.weather?.qweatherKey ?? ""
        }
    }

    private func savedLocationList(_ store: WeatherStore) -> some View {
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
}

// MARK: - 关于

private struct AboutTab: View {
    @Environment(\.theme) private var theme

    private var version: String {
        Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?"
    }

    var body: some View {
        VStack(spacing: 10) {
            Spacer()
            Image(nsImage: StatsGlyphs.makeBattery(fraction: 0.93, charging: false, value: ""))
                .opacity(0.9)
            Text("AuraBar")
                .font(.system(.title2, design: .rounded).weight(.semibold))
            Text("版本 \(version)")
                .font(.callout)
                .foregroundStyle(theme.textSecondary)
            Text("轻量精致的 macOS 菜单栏工具 · 日历 / 天气 / 系统监控")
                .font(.caption)
                .foregroundStyle(theme.textSecondary)
            Spacer()
            Divider().overlay(theme.cardBorder)
            HStack {
                Spacer()
                Button("退出 AuraBar", role: .destructive, action: quitApp)
            }
        }
        .frame(maxWidth: .infinity)
    }
}
