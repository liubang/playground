import AppKit
import CoreGraphics
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

/// Separate assertions let a locked/inactive session release display
/// sleep prevention without ever dropping the idle-system-sleep assertion.
/// Activity operations are injectable so tests never change machine power policy.
@MainActor
final class BatterySleepPrevention {
    struct SessionState: Equatable {
        var screenLocked: Bool
        var isActive: Bool

        static func current() -> Self {
            guard let session = CGSessionCopyCurrentDictionary() as? [String: Any] else {
                // No usable session: do not keep a login/lock screen lit.
                return Self(screenLocked: true, isActive: false)
            }
            return Self(
                // This WindowServer key and the distributed lock/unlock
                // notifications are not exposed as named SDK constants.
                screenLocked: session["CGSSessionScreenIsLocked"] as? Bool ?? false,
                isActive: session[kCGSessionOnConsoleKey as String] as? Bool ?? false,
            )
        }
    }

    private var enabled = false
    private var sessionState: SessionState
    private let beginActivity: (ProcessInfo.ActivityOptions, String) -> NSObjectProtocol
    private let endActivity: (NSObjectProtocol) -> Void
    private var systemActivity: NSObjectProtocol?
    private var displayActivity: NSObjectProtocol?

    init(
        sessionState: SessionState,
        beginActivity: @escaping (ProcessInfo.ActivityOptions, String) -> NSObjectProtocol = {
            ProcessInfo.processInfo.beginActivity(options: $0, reason: $1)
        },
        endActivity: @escaping (NSObjectProtocol) -> Void = {
            ProcessInfo.processInfo.endActivity($0)
        },
    ) {
        self.sessionState = sessionState
        self.beginActivity = beginActivity
        self.endActivity = endActivity
    }

    func setEnabled(_ enabled: Bool) {
        self.enabled = enabled
        updateActivities()
    }

    func setScreenLocked(_ locked: Bool) {
        sessionState.screenLocked = locked
        updateActivities()
    }

    func setSessionActive(_ active: Bool) {
        sessionState.isActive = active
        updateActivities()
    }

    func updateSessionState(_ state: SessionState) {
        sessionState = state
        updateActivities()
    }

    deinit {
        if let displayActivity {
            endActivity(displayActivity)
        }
        if let systemActivity {
            endActivity(systemActivity)
        }
    }

    private func updateActivities() {
        if enabled, systemActivity == nil {
            // Plain userInitiated includes idleSystemSleepDisabled.
            // AppNapDisabler must keep using the allowing-idle-sleep variant.
            systemActivity = beginActivity(.userInitiated, "AuraBar 防止系统自动休眠")
        }

        if enabled, sessionState.isActive, !sessionState.screenLocked {
            if displayActivity == nil {
                displayActivity = beginActivity(.idleDisplaySleepDisabled, "AuraBar 未锁屏时保持常亮")
            }
        } else if let activity = displayActivity {
            endActivity(activity)
            displayActivity = nil
        }

        if !enabled, let activity = systemActivity {
            endActivity(activity)
            systemActivity = nil
        }
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

    /// When true, idle system sleep is always prevented; display sleep is
    /// prevented only while the session is active and unlocked. Persisted
    /// across launches and (re)acquired at startup when still on.
    @Published var preventSleep: Bool {
        didSet {
            guard preventSleep != oldValue else { return }
            UserDefaults.standard.set(preventSleep, forKey: Self.preventSleepKey)
            sleepPrevention.setEnabled(preventSleep)
        }
    }

    private var powerSourceRunLoopSource: CFRunLoopSource?
    private let sleepPrevention: BatterySleepPrevention
    private var sessionObservers: [NSObjectProtocol] = []
    private var workspaceObservers: [NSObjectProtocol] = []

    /// Refreshes only matter while the module is visible (status item
    /// inserted or popover open). The 30s fallback timer follows this;
    /// the power-source run loop source stays registered but no-ops —
    /// the events are rare and the callback is cheap. Every visibility
    /// change while visible also refreshes immediately: the fallback
    /// timer may have been paused for a long stretch.
    private lazy var sampler = SamplingController(
        interval: 30,
        tolerance: 10,
        onTick: { [weak self] in self?.refresh() },
        onVisibilityEvent: { [weak self] in self?.refresh() },
    )

    private var samplingActive: Bool {
        sampler.isActive
    }

    func statusItemVisibilityChanged(_ visible: Bool) {
        sampler.statusItemVisibilityChanged(visible)
    }

    func popoverVisibilityChanged(_ open: Bool) {
        sampler.popoverVisibilityChanged(open)
    }

    init() {
        preventSleep = UserDefaults.standard.bool(forKey: Self.preventSleepKey)
        sleepPrevention = BatterySleepPrevention(
            sessionState: .init(screenLocked: true, isActive: false),
        )
        // Subscribe before sampling so a lock during startup cannot be lost
        // between the snapshot and notification registration.
        observeSessionChanges()
        sleepPrevention.updateSessionState(.current())
        sleepPrevention.setEnabled(preventSleep)
        // One upfront read regardless of visibility: AppRegistry's
        // hasBattery check (and the popover's empty state) need it.
        refresh()
        observePowerSourceChanges()
        // Sleep may span a power-state change whose notification never
        // replays on wake — refresh immediately so the label catches up
        // without waiting for the next timer tick.
        let wakeObserver = NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.didWakeNotification,
            object: nil,
            queue: .main,
        ) { [weak self] _ in
            Task { @MainActor in
                guard let self else { return }
                // Power policy must update even when battery sampling is hidden.
                self.sleepPrevention.updateSessionState(.current())
                if self.samplingActive {
                    self.refresh()
                }
            }
        }
        workspaceObservers.append(wakeObserver)
    }

    deinit {
        for observer in sessionObservers {
            DistributedNotificationCenter.default().removeObserver(observer)
        }
        for observer in workspaceObservers {
            NSWorkspace.shared.notificationCenter.removeObserver(observer)
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
            Task { @MainActor in
                guard store.samplingActive else { return }
                store.refresh()
            }
        }, context)?.takeRetainedValue() else { return }
        CFRunLoopAddSource(CFRunLoopGetMain(), source, .commonModes)
        powerSourceRunLoopSource = source
    }

    private func refresh() {
        info = BatterySampler.info()
    }

    /// Locking is not the same as display sleep or session deactivation.
    /// Listen for both locks and fast user switching; none of these observers
    /// depend on the module's visibility or its sampling timer.
    private func observeSessionChanges() {
        let distributed = DistributedNotificationCenter.default()
        for (name, locked) in [
            ("com.apple.screenIsLocked", true),
            ("com.apple.screenIsUnlocked", false),
        ] {
            let observer = distributed.addObserver(
                forName: Notification.Name(name),
                object: nil,
                queue: .main,
            ) { [weak self] _ in
                Task { @MainActor in
                    self?.sleepPrevention.setScreenLocked(locked)
                }
            }
            sessionObservers.append(observer)
        }

        let workspace = NSWorkspace.shared.notificationCenter
        let resignObserver = workspace.addObserver(
            forName: NSWorkspace.sessionDidResignActiveNotification,
            object: nil,
            queue: .main,
        ) { [weak self] _ in
            Task { @MainActor in
                self?.sleepPrevention.setSessionActive(false)
            }
        }
        let activeObserver = workspace.addObserver(
            forName: NSWorkspace.sessionDidBecomeActiveNotification,
            object: nil,
            queue: .main,
        ) { [weak self] _ in
            Task { @MainActor in
                // An active session can still be locked: resample both flags.
                self?.sleepPrevention.updateSessionState(.current())
            }
        }
        workspaceObservers.append(contentsOf: [resignObserver, activeObserver])
    }
}
