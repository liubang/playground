import AppKit
import Foundation

/// Owns the stats state shared by the three status items (CPU / memory /
/// network): readings, short history windows for the sparklines, and the
/// per-core and memory breakdowns shown in the popovers.
///
/// Sampling cadence: a 2-second repeating timer with a wide tolerance so
/// it coalesces with other system work. All sampling is cheap Mach calls
/// — microseconds per tick.
@MainActor
final class SystemStatsStore: ObservableObject {
    // MARK: - CPU

    /// 0...1, across all cores.
    @Published private(set) var cpuUsage = 0.0
    /// 0...1 per core, in core order.
    @Published private(set) var perCoreUsage: [Double] = []

    // MARK: - Memory

    @Published private(set) var memoryUsed: UInt64 = 0
    @Published private(set) var memoryTotal: UInt64 = 1
    @Published private(set) var memoryApp: UInt64 = 0
    @Published private(set) var memoryWired: UInt64 = 0
    @Published private(set) var memoryCompressed: UInt64 = 0

    // MARK: - Network

    /// Bytes per second.
    @Published private(set) var upRate = 0.0
    /// Bytes per second.
    @Published private(set) var downRate = 0.0
    /// Cumulative bytes since boot.
    @Published private(set) var upTotal: UInt64 = 0
    /// Cumulative bytes since boot.
    @Published private(set) var downTotal: UInt64 = 0

    /// Active interfaces (Wi-Fi, ethernet…) ordered with the default-
    /// route one first, refreshed every ~10s while any stats popover is
    /// visible.
    @Published private(set) var interfaces: [SystemSampler.InterfaceInfo] = []
    /// Best-effort public IP, refreshed every ~10 minutes.
    @Published private(set) var publicIP: String?

    /// Smoothed Y domain for the network chart: jumps up instantly when
    /// traffic spikes, decays slowly afterwards, so the axis doesn't
    /// flicker between samples.
    @Published private(set) var networkYMax: Double = 4096

    // MARK: - Processes

    /// Top 10 by current CPU% (Activity Monitor semantics: can exceed
    /// 100% on multicore).
    @Published private(set) var topCPU: [ProcessSample] = []
    /// Top 10 by resident memory.
    @Published private(set) var topMemory: [ProcessSample] = []

    // MARK: - Histories (last 60 samples ≈ 2 minutes)

    @Published private(set) var cpuHistory: [Double] = []
    @Published private(set) var memoryHistory: [Double] = []
    @Published private(set) var upHistory: [Double] = []
    @Published private(set) var downHistory: [Double] = []

    private static let historyCapacity = 60
    private static let sampleInterval: TimeInterval = 2

    private var timer: Timer?
    private var lastCPU: [SystemSampler.CPUTicks]?
    private var lastNet: (rx: UInt64, tx: UInt64, at: Date)?
    private var lastProcTicks: [Int32: (ticks: UInt64, at: Date)] = [:]
    /// Number of currently open stats popovers (0-3). Histories,
    /// breakdowns and process enumeration only run while > 0 — the menu
    /// bar labels keep updating regardless.
    private var openPopovers = 0
    /// Number of the three stats status items currently inserted (0-3).
    /// All sampling stops while no item is visible and no popover is
    /// open: a hidden module has no label to refresh.
    private var visibleItems = 0
    private var processTick = 0
    private var netInfoTick = 0
    private var publicIPTick = 0

    private var anyPopoverOpen: Bool {
        openPopovers > 0
    }

    private var samplingActive: Bool {
        visibleItems > 0 || anyPopoverOpen
    }

    func statusItemVisibilityChanged(_ visible: Bool) {
        visibleItems += visible ? 1 : -1
        updateSampling()
    }

    func popoverDidOpen() {
        openPopovers += 1
        updateSampling()
        // No immediate sample here: sampling right after the timer's own
        // tick would diff against a tiny dt and inflate the rates into
        // bogus spikes. Histories are always warm anyway (they append
        // regardless of visibility), so the charts open populated.
    }

    func popoverDidClose() {
        openPopovers = max(0, openPopovers - 1)
        updateSampling()
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
        backfillHistories()
        if publicIP == nil {
            fetchPublicIP()
        }
        let t = Timer(timeInterval: Self.sampleInterval, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.sample() }
        }
        t.tolerance = 1
        RunLoop.main.add(t, forMode: .common)
        timer = t
    }

    /// Pre-fills the 2-minute window with the current reading so charts
    /// open full-width with complete axis labels — new samples then slide
    /// in from the right. Runs on every sampling start: after a pause the
    /// frozen pre-pause history would read as live data, so a fresh flat
    /// baseline is more honest than keeping it.
    private func backfillHistories() {
        cpuHistory = Array(repeating: cpuUsage, count: Self.historyCapacity)
        memoryHistory = Array(
            repeating: Double(memoryUsed) / Double(memoryTotal),
            count: Self.historyCapacity,
        )
        upHistory = Array(repeating: upRate, count: Self.historyCapacity)
        downHistory = Array(repeating: downRate, count: Self.historyCapacity)
        updateNetworkYMax()
    }

    // MARK: - Sampling

    private func sample() {
        // Label-critical readings are always sampled; popover-only state
        // (histories, breakdowns, process lists) pauses while no stats
        // popover is open.
        let detailed = anyPopoverOpen

        if let cores = SystemSampler.cpuTicks() {
            if let last = lastCPU, last.count == cores.count {
                var busyAll = 0.0
                var totalAll = 0.0
                var perCore: [Double] = []
                for (new, old) in zip(cores, last) {
                    let busyDelta = Double(new.busy - old.busy)
                    let totalDelta = Double(new.total - old.total)
                    busyAll += busyDelta
                    totalAll += totalDelta
                    perCore.append(totalDelta > 0 ? busyDelta / totalDelta : 0)
                }
                if totalAll > 0 {
                    cpuUsage = busyAll / totalAll
                }
                if detailed {
                    perCoreUsage = perCore
                }
            }
            lastCPU = cores
        }

        if let mem = SystemSampler.memory() {
            memoryUsed = mem.used
            memoryTotal = max(mem.total, 1)
            if detailed {
                memoryApp = mem.app
                memoryWired = mem.wired
                memoryCompressed = mem.compressed
            }
        }

        let net = SystemSampler.networkBytes()
        let now = Date()
        if detailed {
            downTotal = net.rx
            upTotal = net.tx
        }
        if let last = lastNet {
            let dt = now.timeIntervalSince(last.at)
            // A decreasing counter means the 32-bit counter wrapped (or an
            // interface reset) — skip the sample rather than reporting a
            // huge spike. A sub-half-second window exaggerates burst
            // jitter into bogus spikes, so it gets a floor too.
            if dt >= 0.5, net.rx >= last.rx, net.tx >= last.tx {
                // EMA-smooth the instantaneous rate (α=0.4): 2s samples
                // are bursty, and smoothing turns needle spikes into
                // readable bumps — much closer to a Grafana panel.
                let instantDown = Double(net.rx - last.rx) / dt
                let instantUp = Double(net.tx - last.tx) / dt
                downRate = downRate * 0.6 + instantDown * 0.4
                upRate = upRate * 0.6 + instantUp * 0.4
            }
        }
        lastNet = (net.rx, net.tx, now)

        // Histories are ~240 Doubles — appending them is free, and it
        // keeps the charts warm for the next popover open.
        appendHistory()
        updateNetworkYMax()

        // Process enumeration is the only genuinely expensive sampler
        // (~1k syscalls) — run it at half cadence (~4s) and only while
        // a stats popover is visible.
        guard detailed else { return }
        processTick &+= 1
        if processTick % 2 == 1 {
            sampleProcesses()
        }
        // Interface info changes slowly — every ~10s is plenty.
        netInfoTick &+= 1
        if netInfoTick % 5 == 1 {
            Task.detached(priority: .utility) { [weak self] in
                let infos = SystemSampler.activeInterfaces()
                await self?.setInterfaces(infos)
            }
        }
        // Public IP: every ~10 minutes.
        publicIPTick &+= 1
        if publicIPTick % 300 == 1 {
            fetchPublicIP()
        }
    }

    private func setInterfaces(_ infos: [SystemSampler.InterfaceInfo]) {
        interfaces = infos
    }

    private func fetchPublicIP() {
        Task.detached(priority: .utility) { [weak self] in
            for endpoint in publicIPEndpoints {
                guard let url = URL(string: endpoint),
                      let (data, _) = try? await URLSession.shared.data(from: url),
                      let text = String(data: data, encoding: .utf8),
                      let ip = extractIPv4(from: text) else { continue }
                await self?.setPublicIP(ip)
                return
            }
        }
    }

    private func setPublicIP(_ ip: String) {
        publicIP = ip
    }

    // MARK: - Process sampling

    /// Enumerating all processes costs ~1k syscalls; run it off the main
    /// actor and hop back only to publish.
    private func sampleProcesses() {
        Task.detached(priority: .utility) { [weak self] in
            let infos = SystemSampler.processInfos()
            await self?.ingestProcesses(infos, at: Date())
        }
    }

    private func ingestProcesses(_ infos: [SystemSampler.ProcessInfo], at now: Date) {
        var samples: [ProcessSample] = []
        samples.reserveCapacity(infos.count)
        for info in infos {
            var cpu = 0.0
            if let last = lastProcTicks[info.pid], info.cpuTicks >= last.ticks {
                let wall = now.timeIntervalSince(last.at)
                if wall > 0 {
                    cpu = Double(info.cpuTicks - last.ticks) / 1e9 / wall * 100
                }
            }
            lastProcTicks[info.pid] = (info.cpuTicks, now)
            samples.append(ProcessSample(
                pid: info.pid,
                name: info.name,
                cpuPercent: cpu,
                memory: info.memory,
            ))
        }
        let alive = Set(infos.map(\.pid))
        lastProcTicks = lastProcTicks.filter { alive.contains($0.key) }
        topCPU = samples.sorted { $0.cpuPercent > $1.cpuPercent }.prefix(10).map(\.self)
        topMemory = samples.sorted { $0.memory > $1.memory }.prefix(10).map(\.self)
    }

    private func updateNetworkYMax() {
        let current = max(
            upHistory.max() ?? 0,
            downHistory.max() ?? 0,
            1024,
        )
        networkYMax = max(current, networkYMax * 0.94)
    }

    private func appendHistory() {
        cpuHistory.append(cpuUsage)
        memoryHistory.append(Double(memoryUsed) / Double(memoryTotal))
        upHistory.append(upRate)
        downHistory.append(downRate)
        if cpuHistory.count > Self.historyCapacity {
            cpuHistory.removeFirst()
            memoryHistory.removeFirst()
            upHistory.removeFirst()
            downHistory.removeFirst()
        }
    }
}

/// Public IP providers in fallback order: some are unreachable
/// depending on the network (proxy/GFW), first valid IPv4 wins.
private let publicIPEndpoints = [
    "https://api.ipify.org",
    "https://checkip.amazonaws.com",
    "https://myip.ipip.net",
]

/// First IPv4-looking token in the response body (endpoints return
/// either a bare IP or a sentence containing one).
private func extractIPv4(from text: String) -> String? {
    for token in text.split(whereSeparator: { !$0.isNumber && $0 != "." }) {
        let parts = token.split(separator: ".")
        let isIPv4 = parts.count == 4 && parts.allSatisfy { part in
            !part.isEmpty && part.count <= 3
                && part.allSatisfy(\.isNumber)
                && (Int(part) ?? 256) <= 255
        }
        if isIPv4 {
            return String(token)
        }
    }
    return nil
}

/// One row in the popovers' top-10 process lists.
struct ProcessSample: Identifiable, Equatable {
    let pid: Int32
    let name: String
    /// 0-100+ (multicore sum, like Activity Monitor).
    let cpuPercent: Double
    /// Resident bytes.
    let memory: UInt64

    var id: Int32 {
        pid
    }
}

/// Compact number formatting for the menu bar and the popovers.
enum Formatters {
    /// "12G" / "512M" — one decimal below 100, integer above.
    static func bytes(_ value: UInt64) -> String {
        if value == 0 {
            return "0"
        }
        let units = ["B", "K", "M", "G", "T"]
        var v = Double(value)
        var i = 0
        while v >= 1024, i < units.count - 1 {
            v /= 1024
            i += 1
        }
        return v >= 99.95
            ? "\(Int(v.rounded()))\(units[i])"
            : String(format: "%.1f%@", v, units[i])
    }

    /// "1.2M" / "35K" per second.
    static func rate(_ bytesPerSecond: Double) -> String {
        bytes(max(0, UInt64(bytesPerSecond.rounded())))
    }

    /// "37%" / "4.2%" — one decimal below 10, integer above.
    static func percent(_ value: Double) -> String {
        value >= 10
            ? "\(Int(value.rounded()))%"
            : String(format: "%.1f%%", value)
    }

    /// "23.4 / 32 GB" for the memory card.
    static func usagePair(_ used: UInt64, _ total: UInt64) -> String {
        let gb = 1024.0 * 1024.0 * 1024.0
        return String(format: "%.1f / %.0f GB", Double(used) / gb, Double(total) / gb)
    }
}
