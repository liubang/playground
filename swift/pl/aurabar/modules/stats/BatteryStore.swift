import AppKit
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

/// Owns the battery module's state. Refreshes are event-driven via the
/// IOPowerSources run loop source (AC attach/detach, capacity ticks and
/// charging-state changes all fire it — that's how the system menu bar
/// reacts instantly), with a relaxed 30s timer as a fallback for stale
/// time-remaining estimates.
///
/// Also owns the 防休眠 toggle: power management naturally belongs to the
/// battery module (on machines with a battery; the store exists either
/// way), and the toggle prefers ProcessInfo activities over wrapping
/// `caffeinate` so no helper process is needed.
@MainActor
final class BatteryStore: ObservableObject {
    @Published private(set) var info: BatteryInfo?

    /// UserDefaults key persisting the sleep-prevention toggle.
    static let preventSleepKey = "AuraBar.battery.preventSleep"

    /// When true, idle system sleep and display sleep are prevented via a
    /// ProcessInfo activity. Persisted across launches and (re)acquired
    /// at startup when still on.
    @Published var preventSleep: Bool {
        didSet {
            guard preventSleep != oldValue else { return }
            UserDefaults.standard.set(preventSleep, forKey: Self.preventSleepKey)
            if preventSleep {
                startSleepPrevention()
            } else {
                stopSleepPrevention()
            }
        }
    }

    private var timer: Timer?
    private var powerSourceRunLoopSource: CFRunLoopSource?
    private var sleepActivity: NSObjectProtocol?

    init() {
        preventSleep = UserDefaults.standard.bool(forKey: Self.preventSleepKey)
        if preventSleep {
            startSleepPrevention()
        }
        refresh()
        observePowerSourceChanges()
        // Sleep may span a power-state change whose notification never
        // replays on wake — refresh immediately so the label catches up
        // without waiting for the next timer tick.
        NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.didWakeNotification,
            object: nil,
            queue: .main,
        ) { [weak self] _ in
            Task { @MainActor in self?.refresh() }
        }
        let t = Timer(timeInterval: 30, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refresh() }
        }
        t.tolerance = 10
        RunLoop.main.add(t, forMode: .common)
        timer = t
    }

    deinit {
        timer?.invalidate()
        if let activity = sleepActivity {
            ProcessInfo.processInfo.endActivity(activity)
        }
        if let source = powerSourceRunLoopSource {
            CFRunLoopRemoveSource(CFRunLoopGetMain(), source, .commonModes)
        }
    }

    /// Registers for IOKit power-source notifications. The callback fires
    /// on the main run loop whenever the power source blob changes, so AC
    /// plug/unplug is reflected immediately instead of waiting for the
    /// next timer tick.
    private func observePowerSourceChanges() {
        let context = Unmanaged.passUnretained(self).toOpaque()
        guard let source = IOPSNotificationCreateRunLoopSource({ context in
            guard let context else { return }
            let store = Unmanaged<BatteryStore>.fromOpaque(context).takeUnretainedValue()
            Task { @MainActor in store.refresh() }
        }, context)?.takeRetainedValue() else { return }
        CFRunLoopAddSource(CFRunLoopGetMain(), source, .commonModes)
        powerSourceRunLoopSource = source
    }

    private func refresh() {
        info = BatterySampler.info()
    }

    /// Acquires the ProcessInfo activity keeping the machine awake, the
    /// caffeinate -d equivalent: `.userInitiated` asserts against idle
    /// system sleep (the plain variant includes idleSystemSleepDisabled),
    /// and the display-sleep option keeps the screen on too. This is why
    /// AppNapDisabler must use `.userInitiatedAllowingIdleSystemSleep` —
    /// otherwise the app would pin the machine awake unconditionally and
    /// this toggle would be a no-op.
    private func startSleepPrevention() {
        guard sleepActivity == nil else { return }
        sleepActivity = ProcessInfo.processInfo.beginActivity(
            options: [.userInitiated, .idleDisplaySleepDisabled],
            reason: "AuraBar 防休眠",
        )
    }

    private func stopSleepPrevention() {
        guard let activity = sleepActivity else { return }
        ProcessInfo.processInfo.endActivity(activity)
        sleepActivity = nil
    }
}
