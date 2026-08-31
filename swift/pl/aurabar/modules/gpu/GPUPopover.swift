import SwiftUI

/// The GPU popover: device name plus live utilization with a sparkline,
/// and GPU-allocated memory when the driver reports it (unified memory
/// on Apple silicon); the memory card is omitted for discrete GPUs.
struct GPUPopover: View, StatsPopoverContent {
    @ObservedObject var store: GPUStore
    @AppStorage("themePreference") var themePreference = ThemePreference.system.rawValue
    @AppStorage(ThemeKind.key) var themeKind = ThemeKind.everforest.rawValue
    // Subscribed (not read) so an accent change re-renders the popover.
    @AppStorage(AccentColor.key) var accentHex = ""
    @Environment(\.colorScheme) var colorScheme

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            usageCard
            if let memoryUsed = store.memoryUsed {
                memoryCard(memoryUsed)
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
                Label("GPU", systemImage: "memorychip.fill")
                    .font(.caption)
                    .foregroundStyle(theme.textSecondary)
                Spacer()
                Text("\(Int((store.usage * 100).rounded()))%")
                    .font(.system(.title3, design: .rounded).weight(.semibold))
                    .monospacedDigit()
                    .contentTransition(.numericText())
            }
            Text(store.name)
                .font(.caption)
                .foregroundStyle(theme.textSecondary)
            TimeSeriesChart(
                series: [TimeSeriesChart.Series(color: theme.accent, values: store.history, fill: true)],
                maxY: 1,
                yLabel: { "\(Int($0 * 100))" },
                xLabels: chartTimeLabels(count: store.history.count),
            )
        }
        .cardStyle()
    }

    private func memoryCard(_ used: UInt64) -> some View {
        HStack(spacing: 8) {
            Circle()
                .fill(theme.aqua)
                .frame(width: 6, height: 6)
            Text("GPU 内存")
                .font(.callout)
                .foregroundStyle(theme.textSecondary)
            Spacer()
            Text(Formatters.bytes(used))
                .font(.callout)
                .monospacedDigit()
                .foregroundStyle(theme.textPrimary)
        }
        .cardStyle()
    }
}
