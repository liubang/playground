import SwiftUI

/// Shared theme plumbing for the three stats popovers.
protocol StatsPopoverContent: View {
    var themePreference: String { get }
    var colorScheme: ColorScheme { get }
}

extension StatsPopoverContent {
    var theme: Theme {
        (ThemePreference(rawValue: themePreference) ?? .system).theme(for: colorScheme)
    }

    var pinnedColorScheme: ColorScheme? {
        (ThemePreference(rawValue: themePreference) ?? .system).pinnedColorScheme
    }
}

/// The CPU popover: big percentage with a usage sparkline, and a
/// per-core bar breakdown.
struct CPUPopover: View, StatsPopoverContent {
    @ObservedObject var store: SystemStatsStore
    @AppStorage("themePreference") var themePreference = ThemePreference.system.rawValue
    @Environment(\.colorScheme) var colorScheme

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            usageCard
            if !store.perCoreUsage.isEmpty {
                perCoreCard
            }
            if !store.topCPU.isEmpty {
                topProcessesCard
            }
            StatsFooter()
        }
        .padding(12)
        .frame(width: 316)
        .foregroundStyle(theme.textPrimary)
        .background(theme.background)
        .environment(\.theme, theme)
        .preferredColorScheme(pinnedColorScheme)
    }

    private var usageCard: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Label("CPU", systemImage: "cpu")
                    .font(.caption)
                    .foregroundStyle(theme.textSecondary)
                Spacer()
                Text("\(Int((store.cpuUsage * 100).rounded()))%")
                    .font(.system(.title3, design: .rounded).weight(.semibold))
                    .monospacedDigit()
                    .contentTransition(.numericText())
            }
            TimeSeriesChart(
                series: [TimeSeriesChart.Series(color: theme.accent, values: store.cpuHistory, fill: true)],
                maxY: 1,
                yLabel: { "\(Int($0 * 100))" },
                xLabels: chartTimeLabels(count: store.cpuHistory.count),
            )
        }
        .cardStyle()
    }

    private var perCoreCard: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("每核心")
                .font(.caption)
                .foregroundStyle(theme.textSecondary)
            HStack(alignment: .bottom, spacing: 4) {
                ForEach(Array(store.perCoreUsage.enumerated()), id: \.offset) { _, usage in
                    GeometryReader { geo in
                        ZStack(alignment: .bottom) {
                            RoundedRectangle(cornerRadius: 2)
                                .fill(theme.background)
                            RoundedRectangle(cornerRadius: 2)
                                .fill(theme.accent)
                                .frame(height: max(geo.size.height * usage, 2))
                        }
                    }
                    .frame(height: 36)
                }
            }
            .animation(.easeInOut(duration: 0.3), value: store.perCoreUsage)
        }
        .cardStyle()
    }

    private var topProcessesCard: some View {
        TopProcessesCard(
            title: "CPU 占用 TOP 10",
            processes: store.topCPU,
            valueColor: theme.accent,
            valueText: { Formatters.percent($0.cpuPercent) },
            valueRatio: { $0.cpuPercent / max(store.topCPU.first?.cpuPercent ?? 1, 1) },
        )
    }
}

/// The memory popover: usage pair with a fill bar, and the app / wired /
/// compressed breakdown.
struct MemoryPopover: View, StatsPopoverContent {
    @ObservedObject var store: SystemStatsStore
    @AppStorage("themePreference") var themePreference = ThemePreference.system.rawValue
    @Environment(\.colorScheme) var colorScheme

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            usageCard
            breakdownCard
            if !store.topMemory.isEmpty {
                topProcessesCard
            }
            StatsFooter()
        }
        .padding(12)
        .frame(width: 316)
        .foregroundStyle(theme.textPrimary)
        .background(theme.background)
        .environment(\.theme, theme)
        .preferredColorScheme(pinnedColorScheme)
    }

    private var fraction: Double {
        Double(store.memoryUsed) / Double(store.memoryTotal)
    }

    private var usageCard: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Label("内存", systemImage: "memorychip")
                    .font(.caption)
                    .foregroundStyle(theme.textSecondary)
                Spacer()
                Text(Formatters.usagePair(store.memoryUsed, store.memoryTotal))
                    .font(.system(.callout, design: .rounded))
                    .monospacedDigit()
                    .foregroundStyle(theme.textPrimary)
            }
            GeometryReader { geo in
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
                        .frame(width: geo.size.width * fraction)
                }
            }
            .frame(height: 5)
            .animation(.easeInOut(duration: 0.3), value: fraction)
        }
        .cardStyle()
    }

    private var breakdownCard: some View {
        VStack(alignment: .leading, spacing: 7) {
            breakdownRow("应用内存", value: store.memoryApp, color: theme.aqua)
            breakdownRow("联动内存", value: store.memoryWired, color: theme.accent)
            breakdownRow("被压缩", value: store.memoryCompressed, color: theme.orange)
        }
        .cardStyle()
    }

    private var topProcessesCard: some View {
        TopProcessesCard(
            title: "内存占用 TOP 10",
            processes: store.topMemory,
            valueColor: theme.orange,
            valueText: { Formatters.bytes($0.memory) },
            valueRatio: { Double($0.memory) / Double(max(store.topMemory.first?.memory ?? 1, 1)) },
        )
    }

    private func breakdownRow(_ name: String, value: UInt64, color: Color) -> some View {
        HStack(spacing: 8) {
            Circle()
                .fill(color)
                .frame(width: 6, height: 6)
            Text(name)
                .font(.callout)
                .foregroundStyle(theme.textSecondary)
            Spacer()
            Text(Formatters.bytes(value))
                .font(.callout)
                .monospacedDigit()
                .foregroundStyle(theme.textPrimary)
        }
    }
}

/// The network popover: current up/down rates with a dual sparkline,
/// and cumulative totals since boot.
struct NetworkPopover: View, StatsPopoverContent {
    @ObservedObject var store: SystemStatsStore
    @AppStorage("themePreference") var themePreference = ThemePreference.system.rawValue
    @Environment(\.colorScheme) var colorScheme

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            connectionCard
            ratesCard
            totalsCard
            StatsFooter()
        }
        .padding(12)
        .frame(width: 316)
        .foregroundStyle(theme.textPrimary)
        .background(theme.background)
        .environment(\.theme, theme)
        .preferredColorScheme(pinnedColorScheme)
    }

    /// The "connections" card: one row per active interface (Wi-Fi /
    /// ethernet / …), the default-route one marked and sorted first,
    /// plus Wi-Fi signal strength, local and public IP.
    private var connectionCard: some View {
        VStack(alignment: .leading, spacing: 7) {
            if store.interfaces.isEmpty {
                HStack(spacing: 6) {
                    Image(systemName: "network.slash")
                        .foregroundStyle(theme.textSecondary)
                    Text("未连接")
                        .font(.callout)
                        .foregroundStyle(theme.textSecondary)
                }
            } else {
                ForEach(Array(store.interfaces.enumerated()), id: \.offset) { index, info in
                    if index > 0 {
                        Divider().overlay(theme.cardBorder)
                    }
                    interfaceRow(info)
                }
            }
            if let publicIP = store.publicIP {
                HStack {
                    Spacer()
                    Text("公网 \(publicIP)")
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(theme.textSecondary)
                }
            }
        }
        .cardStyle()
    }

    private func interfaceRow(_ info: SystemSampler.InterfaceInfo) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack(spacing: 6) {
                Image(systemName: info.kind == .wifi ? "wifi" : "cable.connector")
                    .foregroundStyle(theme.accent)
                Text(info.title)
                    .font(.callout)
                    .fontWeight(.medium)
                    .lineLimit(1)
                if info.isPrimary {
                    Text("默认")
                        .font(.caption2)
                        .foregroundStyle(theme.accent)
                        .padding(.horizontal, 5)
                        .padding(.vertical, 1)
                        .background(theme.accent.opacity(0.15), in: Capsule())
                }
                Spacer()
                if let rssi = info.rssi {
                    HStack(spacing: 4) {
                        Circle()
                            .fill(signalColor(rssi))
                            .frame(width: 6, height: 6)
                        Text("\(rssi) dBm · \(signalLabel(rssi))")
                    }
                    .font(.caption2)
                    .foregroundStyle(theme.textSecondary)
                }
            }
            Text(info.localIP ?? "—")
                .font(.caption.monospacedDigit())
                .foregroundStyle(theme.textPrimary)
                .padding(.leading, 20)
        }
    }

    private func signalColor(_ rssi: Int) -> Color {
        if rssi >= -60 {
            return theme.ok
        }
        if rssi >= -70 {
            return theme.warning
        }
        return theme.rest
    }

    private func signalLabel(_ rssi: Int) -> String {
        if rssi >= -50 {
            return "极强"
        }
        if rssi >= -60 {
            return "良好"
        }
        if rssi >= -70 {
            return "一般"
        }
        return "较差"
    }

    private var ratesCard: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Label("网络", systemImage: "network")
                    .font(.caption)
                    .foregroundStyle(theme.textSecondary)
                Spacer()
                HStack(spacing: 8) {
                    Text("↑\(Formatters.rate(store.upRate))")
                        .foregroundStyle(theme.orange)
                    Text("↓\(Formatters.rate(store.downRate))")
                        .foregroundStyle(theme.accent)
                }
                .font(.system(.callout, design: .rounded))
                .monospacedDigit()
                .contentTransition(.numericText())
            }
            TimeSeriesChart(
                series: [
                    TimeSeriesChart.Series(color: theme.accent, values: store.downHistory, fill: true),
                    TimeSeriesChart.Series(color: theme.orange, values: store.upHistory),
                ],
                maxY: store.networkYMax,
                yLabel: { Formatters.rate($0) },
                xLabels: chartTimeLabels(count: store.downHistory.count),
            )
        }
        .cardStyle()
    }

    private var totalsCard: some View {
        VStack(alignment: .leading, spacing: 7) {
            totalRow("累计下载", value: store.downTotal, color: theme.accent)
            totalRow("累计上传", value: store.upTotal, color: theme.aqua)
        }
        .cardStyle()
    }

    private func totalRow(_ name: String, value: UInt64, color: Color) -> some View {
        HStack(spacing: 8) {
            Circle()
                .fill(color)
                .frame(width: 6, height: 6)
            Text(name)
                .font(.callout)
                .foregroundStyle(theme.textSecondary)
            Spacer()
            Text(Formatters.bytes(value))
                .font(.callout)
                .monospacedDigit()
                .foregroundStyle(theme.textPrimary)
        }
    }
}

// MARK: - Shared pieces

private let chartTimeFormatter: DateFormatter = {
    let formatter = DateFormatter()
    formatter.dateFormat = "HH:mm:ss"
    return formatter
}()

/// Four bottom time labels at quarter positions of the history window
/// (indices 0/15/30/45; the newest point at index 59 stays unlabeled).
private func chartTimeLabels(count: Int) -> [String] {
    guard count > 1 else { return [] }
    return [0, 15, 30, 45].map { index in
        let clamped = min(index, count - 1)
        let date = Date().addingTimeInterval(-Double(count - 1 - clamped) * 2)
        return chartTimeFormatter.string(from: date)
    }
}

/// A top-10 process list card: process name on the left (middle-
/// truncated), a mini proportion bar and the colored value on the
/// right.
private struct TopProcessesCard: View {
    let title: String
    let processes: [ProcessSample]
    let valueColor: Color
    let valueText: (ProcessSample) -> String
    /// 0...1 fill ratio for the proportion bar.
    let valueRatio: (ProcessSample) -> Double

    @Environment(\.theme) private var theme

    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            Text(title)
                .font(.caption)
                .foregroundStyle(theme.textSecondary)
            ForEach(processes) { process in
                HStack(spacing: 8) {
                    Text(process.name)
                        .font(.caption)
                        .foregroundStyle(theme.textPrimary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                    Spacer(minLength: 8)
                    ratioBar(valueRatio(process))
                    Text(valueText(process))
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(valueColor)
                        .frame(minWidth: 46, alignment: .trailing)
                }
            }
        }
        .cardStyle()
    }

    private func ratioBar(_ ratio: Double) -> some View {
        ZStack(alignment: .leading) {
            Capsule().fill(theme.background)
            Capsule()
                .fill(valueColor.opacity(0.75))
                .frame(width: 28 * min(max(ratio, 0), 1))
        }
        .frame(width: 28, height: 3)
    }
}

/// Footer shared by the stats popovers: a cadence note on the left,
/// settings menu on the right. Each module passes its own cadence
/// description — stats show the sampling/window format, battery shows
/// its event-driven refresh model.
struct StatsFooter: View, StatsPopoverContent {
    /// Bottom-left note describing how this module's data refreshes.
    var cadenceLabel = "2s 采样 · 2 分钟窗口"

    @AppStorage("themePreference") var themePreference = ThemePreference.system.rawValue
    @Environment(\.colorScheme) var colorScheme

    var body: some View {
        HStack(spacing: 10) {
            Text(cadenceLabel)
                .font(.caption)
                .foregroundStyle(theme.textSecondary)
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
