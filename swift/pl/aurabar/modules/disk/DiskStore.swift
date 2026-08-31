import Foundation

/// Owns the disk module's state: read/write rates diffed from the
/// driver's cumulative counters, short history windows for the popover
/// chart, and the boot volume's capacity snapshot. Sampling is a few
/// IOKit property reads plus one statfs — cheap enough for the shared
/// 2s cadence, same as the GPU module.
@MainActor
final class DiskStore: ObservableObject {
    /// Bytes per second, EMA-smoothed.
    @Published private(set) var readRate = 0.0
    /// Bytes per second, EMA-smoothed.
    @Published private(set) var writeRate = 0.0
    /// Boot volume capacity; refreshed every sample, republished only
    /// when it actually changed.
    @Published private(set) var volume: DiskSampler.VolumeInfo?
    /// Smoothed Y domain for the rates chart: jumps up instantly when
    /// throughput spikes, decays slowly afterwards (same shaping as the
    /// network chart).
    @Published private(set) var chartYMax: Double = 4096

    /// Last 60 samples ≈ 2 minutes.
    @Published private(set) var readHistory: [Double] = []
    @Published private(set) var writeHistory: [Double] = []

    private static let historyCapacity = 60
    private static let sampleInterval: TimeInterval = 2

    private var timer: Timer?
    private var lastCounters: (read: UInt64, write: UInt64, at: Date)?
    private var itemVisible = false
    private var popoverOpen = false

    /// Sampling runs only while the module can actually be seen: its
    /// status item is inserted, or its popover is open. A hidden module
    /// has no label to refresh and no chart to keep warm.
    private var samplingActive: Bool {
        itemVisible || popoverOpen
    }

    func statusItemVisibilityChanged(_ visible: Bool) {
        itemVisible = visible
        updateSampling()
    }

    func popoverVisibilityChanged(_ open: Bool) {
        popoverOpen = open
        updateSampling()
    }

    deinit {
        timer?.invalidate()
    }

    private func updateSampling() {
        if samplingActive {
            startSampling()
        } else {
            timer?.invalidate()
            timer = nil
        }
    }

    private func startSampling() {
        guard timer == nil else { return }
        sample()
        // Pre-fill the 2-minute window with the current reading, so the
        // chart opens full-width from the first second. Redone on every
        // restart: the frozen pre-pause history would read as live data,
        // so a fresh flat baseline is more honest.
        readHistory = Array(repeating: readRate, count: Self.historyCapacity)
        writeHistory = Array(repeating: writeRate, count: Self.historyCapacity)
        let t = Timer(timeInterval: Self.sampleInterval, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.sample() }
        }
        t.tolerance = 1
        RunLoop.main.add(t, forMode: .common)
        timer = t
    }

    private func sample() {
        // statfs is a single syscall; publishing only on change keeps
        // the popover from re-rendering when nothing moved.
        if let next = DiskSampler.bootVolume(), volume != next {
            volume = next
        }

        // A missing driver set keeps the last reading rather than
        // blanking the label; a decreasing total means a driver left
        // (disk ejected) or a counter wrapped — skip the sample instead
        // of reporting a huge bogus rate.
        guard let counters = DiskSampler.bytes() else { return }
        let now = Date()
        if let last = lastCounters,
           case let dt = now.timeIntervalSince(last.at),
           dt >= 0.5, counters.read >= last.read, counters.write >= last.write
        {
            // EMA-smooth the instantaneous rates (α=0.4), identical to
            // the network rates: 2s samples are bursty and smoothing
            // turns needle spikes into readable bumps.
            let instantRead = Double(counters.read - last.read) / dt
            let instantWrite = Double(counters.write - last.write) / dt
            readRate = readRate * 0.6 + instantRead * 0.4
            writeRate = writeRate * 0.6 + instantWrite * 0.4
        }
        lastCounters = (counters.read, counters.write, now)

        readHistory.append(readRate)
        writeHistory.append(writeRate)
        if readHistory.count > Self.historyCapacity {
            readHistory.removeFirst()
            writeHistory.removeFirst()
        }
        chartYMax = max(
            max(readHistory.max() ?? 0, writeHistory.max() ?? 0, 1024),
            chartYMax * 0.94,
        )
    }
}
