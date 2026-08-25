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

    init() {
        sample()
        let t = Timer(timeInterval: Self.sampleInterval, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.sample() }
        }
        t.tolerance = 1
        RunLoop.main.add(t, forMode: .common)
        timer = t
    }

    // MARK: - Sampling

    private func sample() {
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
                perCoreUsage = perCore
            }
            lastCPU = cores
        }

        if let mem = SystemSampler.memory() {
            memoryUsed = mem.used
            memoryTotal = max(mem.total, 1)
            memoryApp = mem.app
            memoryWired = mem.wired
            memoryCompressed = mem.compressed
        }

        let net = SystemSampler.networkBytes()
        let now = Date()
        downTotal = net.rx
        upTotal = net.tx
        if let last = lastNet {
            let dt = now.timeIntervalSince(last.at)
            // A decreasing counter means the 32-bit counter wrapped (or an
            // interface reset) — skip the sample rather than reporting a
            // huge spike.
            if dt > 0, net.rx >= last.rx, net.tx >= last.tx {
                downRate = Double(net.rx - last.rx) / dt
                upRate = Double(net.tx - last.tx) / dt
            }
        }
        lastNet = (net.rx, net.tx, now)

        appendHistory()
        sampleProcesses()
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
