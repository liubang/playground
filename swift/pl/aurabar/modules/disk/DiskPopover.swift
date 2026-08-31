import SwiftUI

/// The disk popover: read/write rates with a dual sparkline (network
/// widget's layout, drive's semantics), and the boot volume's usage
/// with a fill bar.
struct DiskPopover: View, StatsPopoverContent {
    @ObservedObject var store: DiskStore
    @AppStorage("themePreference") var themePreference = ThemePreference.system.rawValue
    @AppStorage(ThemeKind.key) var themeKind = ThemeKind.everforest.rawValue
    // Subscribed (not read) so an accent change re-renders the popover.
    @AppStorage(AccentColor.key) var accentHex = ""
    @Environment(\.colorScheme) var colorScheme

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            ratesCard
            if let volume = store.volume {
                volumeCard(volume)
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

    private var ratesCard: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Label("磁盘", systemImage: "internaldrive")
                    .font(.caption)
                    .foregroundStyle(theme.textSecondary)
                Spacer()
                HStack(spacing: 8) {
                    Text("W \(Formatters.rate(store.writeRate))")
                        .foregroundStyle(theme.orange)
                    Text("R \(Formatters.rate(store.readRate))")
                        .foregroundStyle(theme.accent)
                }
                .font(.system(.callout, design: .rounded))
                .monospacedDigit()
                .contentTransition(.numericText())
            }
            TimeSeriesChart(
                series: [
                    TimeSeriesChart.Series(color: theme.accent, values: store.readHistory, fill: true),
                    TimeSeriesChart.Series(color: theme.orange, values: store.writeHistory),
                ],
                maxY: store.chartYMax,
                yLabel: { Formatters.rate($0) },
                xLabels: chartTimeLabels(count: store.readHistory.count),
            )
        }
        .cardStyle()
    }

    private func volumeCard(_ volume: DiskSampler.VolumeInfo) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Label(volume.name, systemImage: "internaldrive.fill")
                    .font(.caption)
                    .foregroundStyle(theme.textSecondary)
                Spacer()
                Text(Formatters.usagePair(volume.used, volume.total))
                    .font(.system(.callout, design: .rounded))
                    .monospacedDigit()
                    .foregroundStyle(theme.textPrimary)
            }
            let fraction = Double(volume.used) / Double(max(volume.total, 1))
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
            HStack {
                Spacer()
                Text("可用 \(Formatters.bytes(volume.available))")
                    .font(.caption)
                    .monospacedDigit()
                    .foregroundStyle(theme.textSecondary)
            }
        }
        .cardStyle()
    }
}
