import Foundation
import IOKit
import IOKit.ps

/// Battery snapshot from IOKit power sources plus the smart-battery
/// registry entry (cycle count, design vs. max capacity).
struct BatteryInfo: Equatable, Sendable {
    /// 0-100.
    var percentage: Int
    var isCharging: Bool
    var onAC: Bool
    /// Minutes to full when charging, to empty when discharging.
    var timeRemaining: Int?
    var cycleCount: Int?
    /// 0...1, max capacity over design capacity.
    var health: Double?
}

/// Reads the internal battery via IOPowerSources and IORegistry. Returns
/// nil on machines without a battery (Mac mini, Mac Studio…).
enum BatterySampler {
    static func info() -> BatteryInfo? {
        guard let blob = IOPSCopyPowerSourcesInfo()?.takeRetainedValue(),
              let sources = IOPSCopyPowerSourcesList(blob)?.takeRetainedValue() as? [CFTypeRef]
        else {
            return nil
        }

        var info = BatteryInfo(
            percentage: 0,
            isCharging: false,
            onAC: false,
            timeRemaining: nil,
            cycleCount: nil,
            health: nil,
        )
        var found = false
        for source in sources {
            guard let desc = IOPSGetPowerSourceDescription(blob, source)?
                .takeUnretainedValue() as? [String: Any],
                let type = desc[kIOPSTypeKey] as? String,
                type == kIOPSInternalBatteryType else { continue }
            found = true
            let current = desc[kIOPSCurrentCapacityKey] as? Int ?? 0
            let maxCap = max(desc[kIOPSMaxCapacityKey] as? Int ?? 100, 1)
            info.percentage = Int((Double(current) / Double(maxCap) * 100).rounded())
            info.isCharging = desc[kIOPSIsChargingKey] as? Bool ?? false
            info.onAC = (desc[kIOPSPowerSourceStateKey] as? String) == kIOPSACPowerValue
            // kIOPSTimeToFullKey / kIOPSTimeToEmptyKey are CFSTR macros
            // Swift doesn't import — use the literal keys.
            if info.isCharging {
                let minutes = desc["TimeToFull"] as? Int
                info.timeRemaining = (minutes ?? 0) > 0 ? minutes : nil
            } else {
                let minutes = desc["TimeToEmpty"] as? Int
                info.timeRemaining = (minutes ?? 0) > 0 ? minutes : nil
            }
            break
        }
        guard found else { return nil }

        // Cycle count and design capacity live in the registry entry.
        let service = IOServiceGetMatchingService(
            kIOMainPortDefault,
            IOServiceMatching("AppleSmartBattery"),
        )
        if service != 0 {
            var props: Unmanaged<CFMutableDictionary>?
            if IORegistryEntryCreateCFProperties(service, &props, kCFAllocatorDefault, 0) == KERN_SUCCESS,
               let dict = props?.takeRetainedValue() as? [String: Any]
            {
                info.cycleCount = dict["CycleCount"] as? Int
                // Health = current max charge / design capacity. On modern
                // Macs MaxCapacity is a *percentage*, not mAh — the mAh
                // figures live in AppleRawMaxCapacity /
                // NominalChargeCapacity.
                let design = dict["DesignCapacity"] as? Int
                let rawMax = dict["AppleRawMaxCapacity"] as? Int
                    ?? dict["NominalChargeCapacity"] as? Int
                if let design, design > 0, let rawMax {
                    info.health = min(Double(rawMax) / Double(design), 1)
                }
            }
            IOObjectRelease(service)
        }
        return info
    }
}

/// Owns the battery module's state, refreshed on a relaxed 30s timer —
/// charge level and AC state change slowly, and IOKit reads are cheap.
@MainActor
final class BatteryStore: ObservableObject {
    @Published private(set) var info: BatteryInfo?

    private var timer: Timer?

    init() {
        refresh()
        let t = Timer(timeInterval: 30, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refresh() }
        }
        t.tolerance = 10
        RunLoop.main.add(t, forMode: .common)
        timer = t
    }

    private func refresh() {
        info = BatterySampler.info()
    }
}
