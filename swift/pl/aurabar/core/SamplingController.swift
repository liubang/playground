import Foundation

/// A store whose background work follows module visibility. Every
/// module store already exposes these two methods; the protocol lets
/// AppDelegate wire both visibility streams generically.
@MainActor
protocol VisibilityDriven: AnyObject {
    func statusItemVisibilityChanged(_ visible: Bool)
    func popoverVisibilityChanged(_ open: Bool)
}

extension SystemStatsStore: VisibilityDriven {}
extension BatteryStore: VisibilityDriven {}
extension DiskStore: VisibilityDriven {}
extension GPUStore: VisibilityDriven {}
extension WeatherStore: VisibilityDriven {}

/// Shared "sample only while visible" driver for the module stores.
///
/// The timer runs while at least one visibility source holds — an
/// inserted status item or an open popover — and stops when none does,
/// so a hidden module never burns cycles (or hits the network) for UI
/// nobody can see. Sources are *counted* (and clamped at zero) rather
/// than boolean: CPU/memory/network are three status items sharing one
/// SystemStatsStore, and a stray unmatched "hidden" event must not
/// silence sampling for the remaining ones.
///
/// Callbacks:
/// - `onStart`: once per inactive→active transition, before the timer
///   is created — immediate sample, history backfill, stale-data
///   refetch, whatever the store needs on wake.
/// - `onTick`: every `interval` while active.
/// - `onVisibilityEvent`: every visibility *change* while active (even
///   when the timer keeps running) — a catch-up hook for stores whose
///   timer is only a fallback, e.g. the event-driven battery store.
@MainActor
final class SamplingController {
    private let interval: TimeInterval
    private let tolerance: TimeInterval
    private let onStart: () -> Void
    private let onTick: () -> Void
    private let onVisibilityEvent: () -> Void

    private var timer: Timer?
    private var visibleItems = 0
    private var openPopovers = 0

    init(
        interval: TimeInterval,
        tolerance: TimeInterval = 1,
        onStart: @escaping () -> Void = {},
        onTick: @escaping () -> Void,
        onVisibilityEvent: @escaping () -> Void = {},
    ) {
        self.interval = interval
        self.tolerance = tolerance
        self.onStart = onStart
        self.onTick = onTick
        self.onVisibilityEvent = onVisibilityEvent
    }

    /// Any popover of the module is currently open.
    var popoverOpen: Bool {
        openPopovers > 0
    }

    /// Sampling should be running: some status item is inserted or
    /// some popover is open.
    var isActive: Bool {
        visibleItems > 0 || openPopovers > 0
    }

    func statusItemVisibilityChanged(_ visible: Bool) {
        visibleItems = max(0, visibleItems + (visible ? 1 : -1))
        update()
    }

    func popoverVisibilityChanged(_ open: Bool) {
        openPopovers = max(0, openPopovers + (open ? 1 : -1))
        update()
    }

    deinit {
        timer?.invalidate()
    }

    private func update() {
        guard isActive else {
            timer?.invalidate()
            timer = nil
            return
        }
        onVisibilityEvent()
        // onStart fires only on the inactive→active transition: when
        // the timer is already running, an extra popover opening must
        // not trigger an out-of-band sample (a tiny diff window would
        // inflate rate readings into bogus spikes).
        guard timer == nil else { return }
        onStart()
        let t = Timer(timeInterval: interval, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.onTick() }
        }
        t.tolerance = tolerance
        RunLoop.main.add(t, forMode: .common)
        timer = t
    }
}
