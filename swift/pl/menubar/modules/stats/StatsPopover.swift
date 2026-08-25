import Charts
import SwiftUI

/// Shared theme plumbing for the three stats popovers.
private protocol StatsPopoverContent: View {
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
            StatsFooter(current: "cpu")
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
            GrafanaChart(
                series: [GrafanaSeries(color: theme.accent, values: store.cpuHistory)],
                maxY: 1,
                yLabel: { "\(Int($0 * 100))" },
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
            StatsFooter(current: "memory")
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
            ratesCard
            totalsCard
            StatsFooter(current: "network")
        }
        .padding(12)
        .frame(width: 316)
        .foregroundStyle(theme.textPrimary)
        .background(theme.background)
        .environment(\.theme, theme)
        .preferredColorScheme(pinnedColorScheme)
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
                        .foregroundStyle(theme.aqua)
                    Text("↓\(Formatters.rate(store.downRate))")
                        .foregroundStyle(theme.accent)
                }
                .font(.system(.callout, design: .rounded))
                .monospacedDigit()
                .contentTransition(.numericText())
            }
            GrafanaChart(
                series: [
                    GrafanaSeries(color: theme.aqua, values: store.upHistory),
                    GrafanaSeries(color: theme.accent, values: store.downHistory),
                ],
                maxY: max(
                    store.upHistory.max() ?? 0,
                    store.downHistory.max() ?? 0,
                    1024,
                ),
                yLabel: { Formatters.rate($0) },
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

/// One colored series in a GrafanaChart.
private struct GrafanaSeries {
    let color: Color
    let values: [Double]
}

/// Grafana-styled time series: subtle grid lines, small axis labels
/// (values on the left, wall-clock times at the bottom), a crisp line
/// with a soft gradient fill underneath. X positions are indices into
/// the 2s-cadence history window, labeled as clock times.
private struct GrafanaChart: View {
    let series: [GrafanaSeries]
    let maxY: Double
    let yLabel: (Double) -> String

    @Environment(\.theme) private var theme

    private static let sampleInterval: TimeInterval = 2
    private static let timeFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss"
        return formatter
    }()

    var body: some View {
        Chart {
            ForEach(Array(series.enumerated()), id: \.offset) { _, item in
                plot(for: item)
            }
        }
        .chartYScale(domain: 0 ... maxY)
        .chartYAxis {
            AxisMarks(position: .leading, values: .automatic(desiredCount: 4)) { value in
                AxisGridLine(stroke: StrokeStyle(lineWidth: 0.5))
                    .foregroundStyle(theme.cardBorder.opacity(0.55))
                AxisValueLabel {
                    if let v = value.as(Double.self) {
                        Text(yLabel(v))
                            .font(.system(size: 8))
                            .foregroundStyle(theme.textSecondary)
                    }
                }
            }
        }
        .chartXAxis {
            AxisMarks(values: .stride(by: 15)) { value in
                AxisGridLine(stroke: StrokeStyle(lineWidth: 0.5))
                    .foregroundStyle(theme.cardBorder.opacity(0.55))
                AxisValueLabel {
                    if let index = value.as(Int.self) {
                        Text(xLabel(index))
                            .font(.system(size: 8))
                            .foregroundStyle(theme.textSecondary)
                    }
                }
            }
        }
        .frame(height: 64)
    }

    private func xLabel(_ index: Int) -> String {
        let count = series.map(\.values.count).max() ?? 0
        let date = Date().addingTimeInterval(-Double(count - 1 - index) * Self.sampleInterval)
        return Self.timeFormatter.string(from: date)
    }

    /// One series' area + line, extracted so the compiler can type-check
    /// the chart content in reasonable time.
    @ChartContentBuilder
    private func plot(for item: GrafanaSeries) -> some ChartContent {
        ForEach(item.values.indices, id: \.self) { index in
            AreaMark(x: .value("t", index), y: .value("v", item.values[index]))
                .interpolationMethod(.catmullRom)
                .foregroundStyle(
                    LinearGradient(
                        colors: [item.color.opacity(0.22), item.color.opacity(0.02)],
                        startPoint: .top,
                        endPoint: .bottom,
                    ),
                )
            LineMark(x: .value("t", index), y: .value("v", item.values[index]))
                .interpolationMethod(.catmullRom)
                .foregroundStyle(item.color)
                .lineStyle(StrokeStyle(lineWidth: 1.4))
        }
    }
}

/// A top-10 process list card: process name on the left (middle-
/// truncated), colored value on the right.
private struct TopProcessesCard: View {
    let title: String
    let processes: [ProcessSample]
    let valueColor: Color
    let valueText: (ProcessSample) -> String

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
                    Text(valueText(process))
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(valueColor)
                }
            }
        }
        .cardStyle()
    }
}

/// Footer shared by the three stats popovers: cadence note on the left,
/// settings menu on the right.
private struct StatsFooter: View, StatsPopoverContent {
    /// Module identifier for ModuleToggles ("cpu"/"memory"/"network").
    let current: String

    @AppStorage("themePreference") var themePreference = ThemePreference.system.rawValue
    @Environment(\.colorScheme) var colorScheme
    @State private var settingsError: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            if let settingsError {
                Text(settingsError)
                    .font(.caption)
                    .foregroundStyle(theme.rest)
            }
            HStack(spacing: 10) {
                Text("2s 采样 · 2 分钟窗口")
                    .font(.caption)
                    .foregroundStyle(theme.textSecondary)
                Spacer()
                settingsMenu
            }
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
            ModuleToggles(current: current)
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
