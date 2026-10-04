import AppKit
import SwiftUI

/// The standalone settings window, styled after macOS System Settings:
/// an icon sidebar on the left, themed section cards with icon-badge
/// rows on the right. Replaces the cramped per-popover gear menus.
struct SettingsView: View {
    /// The single source of truth for the window's content size —
    /// SettingsWindowController sizes the window from this too.
    static let preferredSize = NSSize(width: 640, height: 520)

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
    @State private var hoveredTab: Tab?

    @AppStorage("themePreference") private var themePreference = ThemePreference.system.rawValue
    @AppStorage(ThemeKind.key) private var themeKind = ThemeKind.everforest.rawValue
    @AppStorage(AccentColor.key) private var accentHex = ""
    @Environment(\.colorScheme) private var colorScheme

    private var theme: Theme {
        (ThemePreference(rawValue: themePreference) ?? .system).theme(
            for: colorScheme,
            kind: ThemeKind(rawValue: themeKind) ?? .everforest,
            accentOverride: Color(hexString: accentHex),
        )
    }

    private var pinnedColorScheme: ColorScheme? {
        (ThemePreference(rawValue: themePreference) ?? .system).pinnedColorScheme
    }

    var body: some View {
        HStack(spacing: 0) {
            sidebar
            Divider().overlay(theme.cardBorder)
            content
        }
        .frame(width: Self.preferredSize.width, height: Self.preferredSize.height)
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
                        } else if hoveredTab == tab {
                            RoundedRectangle(cornerRadius: 6)
                                .fill(theme.textPrimary.opacity(0.06))
                        }
                    }
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .onHover { hovering in
                    hoveredTab = hovering ? tab : nil
                }
                .animation(.easeOut(duration: 0.12), value: hoveredTab)
                .accessibilityAddTraits(selected == tab ? .isSelected : [])
            }
            Spacer()
            Text("AuraBar · v\(appVersion())")
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
        // The tab title stays pinned above the scroll area, like the
        // toolbar title in System Settings — scrolling content must
        // not take the page's identity with it.
        VStack(alignment: .leading, spacing: 0) {
            Text(selected.label)
                .font(.system(.title3, design: .rounded).weight(.semibold))
                .padding(.horizontal, 18)
                .padding(.top, 16)
                .padding(.bottom, 10)
            ScrollView {
                VStack(alignment: .leading, spacing: 14) {
                    switch selected {
                    case .general: GeneralTab()
                    case .calendar: CalendarTab()
                    case .weather: WeatherTab()
                    case .about: AboutTab()
                    }
                }
                .padding(.horizontal, 18)
                .padding(.bottom, 18)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        .background(theme.background)
    }
}

/// The marketing version string for display ("0.2.0").
private func appVersion() -> String {
    Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?"
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

/// A themed segmented control. SwiftUI's `.segmented` Picker ignores
/// tint on macOS and always renders the system accent blue, which
/// clashes with the active palette — this one follows theme.accent,
/// matching the sidebar's selection styling.
private struct ThemedSegmented<Option: Hashable>: View {
    let options: [Option]
    let label: (Option) -> String
    @Binding var selection: Option

    @Environment(\.theme) private var theme

    var body: some View {
        HStack(spacing: 2) {
            ForEach(options, id: \.self) { option in
                Button {
                    selection = option
                } label: {
                    Text(label(option))
                        .font(.callout)
                        .foregroundStyle(selection == option ? theme.accent : theme.textPrimary)
                        .padding(.horizontal, 12)
                        .padding(.vertical, 4)
                        .frame(maxWidth: .infinity)
                        .background {
                            if selection == option {
                                RoundedRectangle(cornerRadius: 5)
                                    .fill(theme.accent.opacity(0.2))
                            }
                        }
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityAddTraits(selection == option ? .isSelected : [])
            }
        }
        .padding(2)
        .background(theme.textPrimary.opacity(0.08), in: RoundedRectangle(cornerRadius: 7))
        .animation(.easeInOut(duration: 0.15), value: selection)
    }
}

/// Accent-color picker: theme default (empty string), curated presets,
/// then a free ColorPicker well. Persisted as #RRGGBB in UserDefaults;
/// ThemePreference.theme(for:) applies it to every popover.
private struct AccentSwatches: View {
    @AppStorage(AccentColor.key) private var accentHex = ""
    @AppStorage(ThemeKind.key) private var themeKind = ThemeKind.everforest.rawValue
    /// The environment theme may already carry the override; the
    /// default swatch needs the palette's pristine accent.
    @Environment(\.colorScheme) private var colorScheme
    @Environment(\.theme) private var theme

    private static let presets = [
        "#7FBBB3", "#0A84FF", "#7D7AFF", "#BF5AF2",
        "#FF6482", "#FF9F0A", "#FFD60A", "#A7C080",
    ]

    /// The selected palette's pristine accent for the first swatch.
    private var defaultAccent: Color {
        let kind = ThemeKind(rawValue: themeKind) ?? .everforest
        let pristine = colorScheme == .dark ? kind.darkTheme : (kind.lightTheme ?? kind.darkTheme)
        return pristine.accent
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
            // A conic-gradient dot reads as "custom color" at a glance;
            // the actual ColorPicker well sits on top, nearly invisible
            // but still hit-testable (opacity must stay > 0).
            ZStack {
                Circle()
                    .fill(
                        AngularGradient(
                            colors: isCustom
                                ? [customColor.wrappedValue, customColor.wrappedValue]
                                : [.red, .orange, .yellow, .green, .cyan, .blue, .purple, .red],
                            center: .center,
                        ),
                    )
                    .frame(width: 16, height: 16)
                    .overlay {
                        if isCustom {
                            selectionRing
                        }
                    }
                ColorPicker("", selection: customColor, supportsOpacity: false)
                    .labelsHidden()
                    .frame(width: 16, height: 16)
                    .clipped()
                    .opacity(0.011)
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
                    if selected {
                        selectionRing
                    }
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

/// Palette picker: one mini card per bundled theme, showing its five
/// signature colors over the palette's own dark background so each
/// swatch previews its own character rather than the live theme's.
private struct ThemeSwatches: View {
    @AppStorage(ThemeKind.key) private var kindRaw = ThemeKind.everforest.rawValue
    @Environment(\.theme) private var theme
    @Environment(\.colorScheme) private var colorScheme

    private var kind: ThemeKind {
        ThemeKind(rawValue: kindRaw) ?? .everforest
    }

    /// Preview each palette on the side of the appearance the user is
    /// actually looking at; palettes without a light variant fall back
    /// to their dark theme.
    private func palette(for candidate: ThemeKind) -> Theme {
        colorScheme == .dark ? candidate.darkTheme : (candidate.lightTheme ?? candidate.darkTheme)
    }

    var body: some View {
        LazyVGrid(
            columns: Array(repeating: GridItem(.flexible(), alignment: .leading), count: 3),
            spacing: 8,
        ) {
            ForEach(ThemeKind.allCases, id: \.rawValue) { candidate in
                card(candidate)
            }
        }
        .padding(.vertical, 8)
    }

    private func card(_ candidate: ThemeKind) -> some View {
        let palette = palette(for: candidate)
        return Button {
            kindRaw = candidate.rawValue
        } label: {
            VStack(alignment: .leading, spacing: 6) {
                HStack(spacing: 4) {
                    let signature = [palette.accent, palette.orange, palette.ok, palette.rest, palette.aqua]
                    ForEach(Array(signature.enumerated()), id: \.offset) { _, color in
                        Circle().fill(color).frame(width: 10, height: 10)
                    }
                    Spacer()
                    Text("Aa")
                        .font(.system(size: 9, weight: .semibold))
                        .foregroundStyle(palette.textPrimary.opacity(0.85))
                }
                .padding(.horizontal, 8)
                .padding(.vertical, 7)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(palette.background, in: RoundedRectangle(cornerRadius: 6))
                Text(candidate.label)
                    .font(.caption)
                    .foregroundStyle(theme.textPrimary)
                    .lineLimit(1)
            }
            .padding(6)
            .background(theme.cardBackground, in: RoundedRectangle(cornerRadius: 8))
            .overlay {
                if candidate == kind {
                    RoundedRectangle(cornerRadius: 8)
                        .stroke(theme.accent, lineWidth: 2)
                }
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }
}

/// Shared layout metrics for the settings rows.
private enum SettingsMetrics {
    /// IconBadge width (22) + SettingsRow spacing (10): the indent
    /// aligning dividers and sub-content with the row's label.
    static let rowIndent: CGFloat = 32
}

/// Divider between rows inside a section card.
private struct RowDivider: View {
    @Environment(\.theme) private var theme

    var body: some View {
        Divider()
            .overlay(theme.cardBorder)
            .padding(.leading, SettingsMetrics.rowIndent)
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
    @AppStorage(ModuleVisibility.gpuKey) private var gpu = true
    @AppStorage(ModuleVisibility.diskKey) private var disk = true
    @AppStorage(ModuleVisibility.batteryKey) private var battery = true

    @Environment(\.theme) private var theme
    @State private var launchError: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            SettingsSection(title: "外观") {
                SettingsRow(icon: "paintpalette", color: theme.accent, label: "外观") {
                    ThemedSegmented(
                        options: ThemePreference.allCases,
                        label: { $0.label },
                        selection: Binding(
                            get: { ThemePreference(rawValue: themePreference) ?? .system },
                            set: { themePreference = $0.rawValue },
                        ),
                    )
                    .frame(width: 220)
                }
                RowDivider()
                SettingsRow(icon: "paintbrush.fill", color: theme.orange, label: "强调色") {
                    AccentSwatches()
                }
            }
            SettingsSection(title: "主题配色") {
                ThemeSwatches()
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
                    moduleToggle("GPU", $gpu, "gpu")
                    moduleToggle("磁盘", $disk, "disk")
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
        var all: [String: Bool] = [
            "calendar": calendar,
            "weather": weather,
            "cpu": cpu,
            "memory": memory,
            "network": network,
            "gpu": gpu,
            "disk": disk,
            "battery": battery,
        ]
        // On battery-less machines the module never exists — counting
        // its (default-true) flag would defeat the guard entirely.
        if !AppRegistry.hasBattery {
            all["battery"] = nil
        }
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
                    .tint(theme.accent)
                }
                RowDivider()
                SettingsRow(icon: "globe", color: theme.aqua, label: "第二时区") {
                    Picker("第二时区", selection: secondTimeZone) {
                        ForEach(SecondTimeZone.allCases, id: \.rawValue) { zone in
                            Text(zone.label).tag(zone.rawValue)
                        }
                    }
                    .labelsHidden()
                    .tint(theme.accent)
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
                    .tint(theme.accent)
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
                        ThemedSegmented(
                            options: WeatherProviderKind.allCases,
                            label: { $0.label },
                            selection: Binding(
                                get: { store.providerKind },
                                set: { store.providerKind = $0 },
                            ),
                        )
                        .frame(width: 280)
                    }
                    if store.providerKind == .apple {
                        Text("需 Apple Developer 账号为 App ID 开启 WeatherKit capability 并用开发者证书签名后生效。")
                            .font(.caption)
                            .foregroundStyle(theme.textSecondary)
                            .padding(.leading, SettingsMetrics.rowIndent)
                            .padding(.vertical, 7)
                    }
                    if store.providerKind == .qweather {
                        RowDivider()
                        SettingsRow(icon: "key", color: theme.orange, label: "和风 Key") {
                            // Return and the button both commit — an
                            // explicit affordance, since a draft is
                            // otherwise lost silently on window close.
                            HStack(spacing: 6) {
                                SettingsField(prompt: "API Key", text: $keyDraft) {
                                    store.qweatherKey = keyDraft
                                }
                                .frame(width: 150)
                                Button("保存") {
                                    store.qweatherKey = keyDraft
                                }
                                .font(.callout)
                                .buttonStyle(.hoverablePlain)
                                .foregroundStyle(theme.accent)
                                .disabled(keyDraft == store.qweatherKey)
                            }
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
                        .padding(.leading, SettingsMetrics.rowIndent)
                        .padding(.vertical, 7)
                    } else {
                        RowDivider()
                        SettingsRow(icon: "plus.circle", color: theme.ok, label: "添加城市") {
                            HStack(spacing: 6) {
                                SettingsField(prompt: "如 北京 / 上海", text: $cityDraft) {
                                    addCity(store)
                                }
                                .frame(width: 120)
                                Button("添加") {
                                    addCity(store)
                                }
                                .font(.callout)
                                .buttonStyle(.hoverablePlain)
                                .foregroundStyle(theme.accent)
                                .disabled(cityDraft.trimmingCharacters(in: .whitespaces).isEmpty)
                            }
                        }
                        if !store.savedLocations.isEmpty {
                            RowDivider()
                            savedLocationList(store)
                                .padding(.leading, SettingsMetrics.rowIndent)
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

    private func addCity(_ store: WeatherStore) {
        Task {
            await store.setCity(cityDraft)
            cityDraft = ""
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
                    .buttonStyle(.hoverablePlain)
                    Spacer()
                    Button {
                        store.removeLocation(loc)
                    } label: {
                        Image(systemName: "xmark")
                            .font(.system(size: 8))
                            .foregroundStyle(theme.textSecondary)
                            .frame(width: 18, height: 18)
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

    var body: some View {
        VStack(spacing: 10) {
            Spacer()
            Image(nsImage: NSApp.applicationIconImage)
                .resizable()
                .frame(width: 64, height: 64)
            Text("AuraBar")
                .font(.system(.title2, design: .rounded).weight(.semibold))
            Text("版本 \(appVersion())")
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
