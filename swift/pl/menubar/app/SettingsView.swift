import SwiftUI

/// The standalone settings window: four tabs — 通用 (theme, modules,
/// launch at login), 日历 (clock format, week start, lunar, second time
/// zone), 天气 (provider, location, API key) and 关于 (version, quit).
/// Replaces the cramped per-popover gear menus.
struct SettingsView: View {
    @AppStorage("themePreference") private var themePreference = ThemePreference.system.rawValue
    @Environment(\.colorScheme) private var colorScheme

    private var theme: Theme {
        (ThemePreference(rawValue: themePreference) ?? .system).theme(for: colorScheme)
    }

    private var pinnedColorScheme: ColorScheme? {
        (ThemePreference(rawValue: themePreference) ?? .system).pinnedColorScheme
    }

    var body: some View {
        TabView {
            GeneralTab()
                .tabItem { Label("通用", systemImage: "gearshape") }
            CalendarTab()
                .tabItem { Label("日历", systemImage: "calendar") }
            WeatherTab()
                .tabItem { Label("天气", systemImage: "cloud.sun") }
            AboutTab()
                .tabItem { Label("关于", systemImage: "info.circle") }
        }
        .padding(20)
        .frame(width: 560, height: 380)
        .background(theme.background)
        .foregroundStyle(theme.textPrimary)
        .environment(\.theme, theme)
        .preferredColorScheme(pinnedColorScheme)
    }
}

/// Section header + content container, themed.
private struct SettingsSection<Content: View>: View {
    let title: String
    @ViewBuilder let content: Content

    @Environment(\.theme) private var theme

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title)
                .font(.caption)
                .foregroundStyle(theme.textSecondary)
            content
        }
        .frame(maxWidth: .infinity, alignment: .leading)
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
        VStack(alignment: .leading, spacing: 16) {
            SettingsSection(title: "外观") {
                Picker("主题", selection: $themePreference) {
                    ForEach(ThemePreference.allCases, id: \.rawValue) { preference in
                        Text(preference.label).tag(preference.rawValue)
                    }
                }
                .labelsHidden()
                .pickerStyle(.segmented)
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
                    moduleToggle("电池", $battery, "battery")
                }
            }
            SettingsSection(title: "系统") {
                Toggle("开机自启", isOn: LaunchAtLogin.binding { launchError = $0 })
                    .tint(theme.accent)
                if let launchError {
                    Text(launchError)
                        .font(.caption)
                        .foregroundStyle(theme.rest)
                }
            }
            Spacer()
        }
    }

    private func moduleToggle(_ label: String, _ binding: Binding<Bool>, _ key: String) -> some View {
        Toggle(label, isOn: binding)
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
        VStack(alignment: .leading, spacing: 16) {
            SettingsSection(title: "菜单栏时钟") {
                Picker("时钟格式", selection: clockFormat) {
                    ForEach(ClockFormat.allCases, id: \.rawValue) { format in
                        Text(format.label).tag(format.rawValue)
                    }
                }
                Picker("第二时区", selection: secondTimeZone) {
                    ForEach(SecondTimeZone.allCases, id: \.rawValue) { zone in
                        Text(zone.label).tag(zone.rawValue)
                    }
                }
            }
            SettingsSection(title: "日历") {
                Picker("每周第一天", selection: $weekStartRaw) {
                    ForEach(WeekStart.allCases, id: \.rawValue) { start in
                        Text(start.label).tag(start.rawValue)
                    }
                }
                Toggle("显示农历与节气", isOn: $showLunar)
                    .tint(theme.accent)
            }
            Spacer()
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
        VStack(alignment: .leading, spacing: 16) {
            if let store = AppRegistry.weather {
                SettingsSection(title: "数据源") {
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

                    if store.providerKind == .qweather {
                        SettingsField(prompt: "和风天气 API Key", text: $keyDraft) {
                            store.qweatherKey = keyDraft
                            Task { await store.refresh() }
                        }
                    }
                }
                SettingsSection(title: "位置") {
                    Toggle("自动定位", isOn: Binding(
                        get: { store.autoLocation },
                        set: { store.setAutoLocation($0) },
                    ))
                    .tint(theme.accent)

                    if store.autoLocation {
                        HStack(spacing: 6) {
                            Image(systemName: "location")
                            Text(store.location.map { "当前：\($0.name)" } ?? "待定位")
                        }
                        .font(.caption)
                        .foregroundStyle(theme.textSecondary)
                    } else {
                        SettingsField(prompt: "添加城市（如 北京 / 上海）", text: $cityDraft) {
                            Task {
                                await store.setCity(cityDraft)
                                cityDraft = ""
                            }
                        }
                        if !store.savedLocations.isEmpty {
                            savedLocationList(store)
                        }
                    }
                }
            }
            Spacer()
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
    }
}
