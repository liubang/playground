import AppKit
import Foundation

/// Menu bar clock label styles. The DateFormatter template is persisted
/// via the raw value of the selected style.
enum ClockFormat: String, CaseIterable, Sendable {
    case full = "M月d日 E HH:mm"
    case compact = "M月d日 HH:mm"
    case timeOnly = "HH:mm"
    case withSeconds = "M月d日 E HH:mm:ss"

    var label: String {
        switch self {
        case .full: "完整（8月23日 周日 14:32）"
        case .compact: "简洁（8月23日 14:32）"
        case .timeOnly: "仅时间（14:32）"
        case .withSeconds: "带秒（8月23日 周日 14:32:05）"
        }
    }

    var showsSeconds: Bool {
        self == .withSeconds
    }
}

/// Owns the menu bar text: a formatted clock refreshed on a timer.
///
/// Two measures keep the label from lagging the wall clock:
/// - App Nap is disabled for the app's lifetime (see AppNapDisabler):
///   as an LSUIElement app with no visible windows, our timers would
///   otherwise be coalesced by minutes whenever the popover is closed.
/// - Each tick is a one-shot timer re-aligned to the next minute (or
///   second) boundary, so any scheduling delay affects at most one tick
///   instead of permanently shifting the cadence — a plain repeating
///   timer can drift to fire at :59.x, displaying the previous minute
///   for most of each minute.
@MainActor
final class MenuBarClock: ObservableObject {
    @Published private(set) var labelText = ""

    @Published var format: ClockFormat {
        didSet {
            UserDefaults.standard.set(format.rawValue, forKey: Self.formatKey)
            restart()
        }
    }

    private static let formatKey = "AuraBar.calendar.clockFormat"
    private var timer: Timer?

    private let formatter: DateFormatter = {
        let f = DateFormatter()
        f.locale = Locale(identifier: "zh_CN")
        return f
    }()

    init() {
        let saved = UserDefaults.standard.string(forKey: Self.formatKey) ?? ""
        format = ClockFormat(rawValue: saved) ?? .full

        NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.didWakeNotification, object: nil, queue: .main,
        ) { [weak self] _ in
            Task { @MainActor in self?.restart() }
        }
        restart()
    }

    private func restart() {
        timer?.invalidate()
        timer = nil
        update()
        scheduleNext()
    }

    /// One-shot self-rescheduling tick aligned to the next boundary.
    private func scheduleNext() {
        let interval: TimeInterval = format.showsSeconds ? 1 : 60
        let now = Date().timeIntervalSince1970
        let delay = max(interval - now.truncatingRemainder(dividingBy: interval), 0.05)
        let t = Timer(fire: Date(timeIntervalSinceNow: delay), interval: 0, repeats: false) { [weak self] _ in
            Task { @MainActor in
                guard let self else { return }
                self.update()
                self.scheduleNext()
            }
        }
        t.tolerance = 0.05
        RunLoop.main.add(t, forMode: .common)
        timer = t
    }

    private func update() {
        formatter.dateFormat = format.rawValue
        labelText = formatter.string(from: Date())
    }
}
