import Foundation

/// Owns the GPU module's state: live utilization, a short history
/// window for the popover sparkline, and GPU-allocated memory when the
/// driver reports it. Sampling is a single IOKit property read — cheap
/// enough to run unconditionally on the shared 2s cadence, same as the
/// CPU/memory/network labels.
@MainActor
final class GPUStore: ObservableObject {
    /// 0...1 device utilization.
    @Published private(set) var usage = 0.0
    /// Display title ("Apple M4 Pro"…).
    @Published private(set) var name = "GPU"
    /// GPU-allocated memory in bytes; nil when the driver doesn't report
    /// one (typical for discrete GPUs).
    @Published private(set) var memoryUsed: UInt64?
    /// Last 60 samples ≈ 2 minutes.
    @Published private(set) var history: [Double] = []

    private static let historyCapacity = 60
    private static let sampleInterval: TimeInterval = 2

    private var timer: Timer?
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
        // popover chart opens full-width from the first second. Redone
        // on every restart: the frozen pre-pause history would read as
        // live data, so a fresh flat baseline is more honest.
        history = Array(repeating: usage, count: Self.historyCapacity)
        let t = Timer(timeInterval: Self.sampleInterval, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.sample() }
        }
        t.tolerance = 1
        RunLoop.main.add(t, forMode: .common)
        timer = t
    }

    private func sample() {
        // No accelerator at all (never on a real Mac): keep the last
        // reading rather than blanking the label.
        guard let info = GPUSampler.info() else { return }
        // The name is static on any given machine — republish only when
        // it actually changes.
        if name != info.name {
            name = info.name
        }
        usage = info.utilization
        memoryUsed = info.memoryUsed
        history.append(info.utilization)
        if history.count > Self.historyCapacity {
            history.removeFirst()
        }
    }
}
