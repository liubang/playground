import SwiftUI

/// The battery popover: a status card (big percentage, charge state,
/// time estimate, level bar), a health card (cycle count, capacity
/// health) and the 防休眠 toggle — keeping the machine awake is a power
/// feature, so it lives here rather than in the settings window.
struct BatteryPopover: View, StatsPopoverContent {
    @ObservedObject var store: BatteryStore
    @AppStorage("themePreference") var themePreference = ThemePreference.system.rawValue
    @Environment(\.colorScheme) var colorScheme

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            if let info = store.info {
                statusCard(info)
                healthCard(info)
            } else {
                Text("未检测到电池（台式机？）")
                    .font(.callout)
                    .foregroundStyle(theme.textSecondary)
                    .frame(maxWidth: .infinity, alignment: .center)
                    .padding(.vertical, 20)
            }
            sleepCard
            StatsFooter(cadenceLabel: "事件驱动 · 实时刷新")
        }
        .padding(12)
        .frame(width: 316)
        .foregroundStyle(theme.textPrimary)
        .background(theme.background)
        .environment(\.theme, theme)
        .preferredColorScheme(pinnedColorScheme)
    }

    // MARK: - Status

    private func statusCard(_ info: BatteryInfo) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Label("电池", systemImage: "battery.75")
                    .font(.caption)
                    .foregroundStyle(theme.textSecondary)
                Spacer()
                Text("\(info.percentage)%")
                    .font(.system(.title3, design: .rounded).weight(.semibold))
                    .monospacedDigit()
                    .contentTransition(.numericText())
            }
            Text(stateLine(info))
                .font(.caption)
                .foregroundStyle(theme.textSecondary)
            if isChargingLimited(info) {
                Button {
                    if let url = URL(
                        string: "x-apple.systempreferences:com.apple.preference.battery",
                    ) {
                        NSWorkspace.shared.open(url)
                    }
                } label: {
                    Label("在系统设置中调整充电上限", systemImage: "bolt.circle")
                        .font(.caption)
                        .foregroundStyle(theme.accent)
                }
                .buttonStyle(.plain)
            }
            let fraction = Double(info.percentage) / 100
            GeometryReader { geo in
                ZStack(alignment: .leading) {
                    Capsule().fill(theme.background)
                    Capsule()
                        .fill(fraction <= 0.2 ? theme.rest : theme.accent)
                        .frame(width: geo.size.width * fraction)
                }
            }
            .frame(height: 5)
            .animation(.easeInOut(duration: 0.3), value: fraction)
        }
        .cardStyle()
    }

    private func stateLine(_ info: BatteryInfo) -> String {
        if info.isCharging {
            if let minutes = info.timeRemaining {
                return "充电中 · 充满还需 \(Self.minutesText(minutes))"
            }
            return "充电中"
        }
        if info.onAC, !info.isCharging, info.percentage < 100 {
            return "已暂停充电 · 系统优化限充中"
        }
        if info.onAC {
            return info.percentage >= 100 ? "已充满 · 电源供电" : "电源供电"
        }
        if let minutes = info.timeRemaining {
            return "放电中 · 预计可用 \(Self.minutesText(minutes))"
        }
        return "放电中"
    }

    // MARK: - Health

    private func healthCard(_ info: BatteryInfo) -> some View {
        VStack(alignment: .leading, spacing: 7) {
            if let cycles = info.cycleCount {
                row("循环次数", value: "\(cycles) 次")
            }
            if let health = info.health {
                row("电池健康", value: "\(Int((health * 100).rounded()))%")
            }
        }
        .cardStyle()
    }

    // MARK: - 防休眠

    /// The sleep-prevention card. Rendered whether or not battery info is
    /// available — staying awake matters on desktops too.
    private var sleepCard: some View {
        HStack(spacing: 8) {
            Image(systemName: store.preventSleep ? "moon.zzz.fill" : "moon.zzz")
                .foregroundStyle(store.preventSleep ? theme.warning : theme.textSecondary)
                .contentTransition(.symbolEffect(.replace))
            VStack(alignment: .leading, spacing: 1) {
                Text("防休眠")
                    .font(.callout)
                Text(store.preventSleep
                    ? "已开启 · 系统不会自动休眠，屏幕保持常亮"
                    : "阻止系统休眠并保持屏幕常亮")
                    .font(.caption2)
                    .foregroundStyle(theme.textSecondary)
                    .contentTransition(.numericText())
            }
            Spacer()
            Toggle(
                "防休眠",
                isOn: Binding(
                    get: { store.preventSleep },
                    set: { store.preventSleep = $0 },
                ),
            )
            .labelsHidden()
            .toggleStyle(.switch)
            .tint(theme.accent)
        }
        .cardStyle()
        .animation(.easeInOut(duration: 0.2), value: store.preventSleep)
    }

    private func row(_ name: String, value: String) -> some View {
        HStack {
            Text(name)
                .font(.callout)
                .foregroundStyle(theme.textSecondary)
            Spacer()
            Text(value)
                .font(.callout)
                .monospacedDigit()
                .foregroundStyle(theme.textPrimary)
        }
    }

    /// On AC but not charging below 100% — macOS is holding the charge
    /// (80% limit or optimized battery charging).
    private func isChargingLimited(_ info: BatteryInfo) -> Bool {
        info.onAC && !info.isCharging && info.percentage < 100
    }

    private static func minutesText(_ minutes: Int) -> String {
        let h = minutes / 60
        let m = minutes % 60
        return h > 0 ? "\(h) 小时 \(m) 分" : "\(m) 分钟"
    }
}
