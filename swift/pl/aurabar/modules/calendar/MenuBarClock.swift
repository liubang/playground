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

/// Optional second time zone appended to the menu bar clock.
enum SecondTimeZone: String, CaseIterable, Sendable {
    case off = ""
    case utc = "UTC"
    case newYork = "America/New_York"
    case losAngeles = "America/Los_Angeles"
    case london = "Europe/London"
    case paris = "Europe/Paris"
    case tokyo = "Asia/Tokyo"
    case singapore = "Asia/Singapore"
    case sydney = "Australia/Sydney"

    var label: String {
        switch self {
        case .off: "关闭"
        case .utc: "UTC"
        case .newYork: "纽约"
        case .losAngeles: "旧金山"
        case .london: "伦敦"
        case .paris: "巴黎"
        case .tokyo: "东京"
        case .singapore: "新加坡"
        case .sydney: "悉尼"
        }
    }

    /// Compact code shown in the menu bar label.
    var code: String {
        switch self {
        case .off: ""
        case .utc: "UTC"
        case .newYork: "NYC"
        case .losAngeles: "SFO"
        case .london: "LON"
        case .paris: "PAR"
        case .tokyo: "TYO"
        case .singapore: "SIN"
        case .sydney: "SYD"
        }
    }

    var timeZone: TimeZone? {
        rawValue.isEmpty ? nil : TimeZone(identifier: rawValue)
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

    /// Second time zone shown after the main clock; "off" hides it.
    @Published var secondTimeZone: SecondTimeZone {
        didSet {
            UserDefaults.standard.set(secondTimeZone.rawValue, forKey: Self.secondTimeZoneKey)
            restart()
        }
    }

    private static let formatKey = "AuraBar.calendar.clockFormat"
    private static let secondTimeZoneKey = "AuraBar.calendar.secondTimeZone"
    private var timer: Timer?
    /// Ticking only matters while the calendar status item is inserted;
    /// a hidden clock doesn't need a minute-aligned timer.
    private var samplingActive = false

    /// Feed from the status item's insertion state.
    func setActive(_ active: Bool) {
        guard active != samplingActive else { return }
        samplingActive = active
        if active {
            restart()
        } else {
            timer?.invalidate()
            timer = nil
        }
    }

    private let formatter: DateFormatter = {
        let f = DateFormatter()
        f.locale = Locale(identifier: "zh_CN")
        return f
    }()

    private let secondFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm"
        return f
    }()

    init() {
        let saved = UserDefaults.standard.string(forKey: Self.formatKey) ?? ""
        format = ClockFormat(rawValue: saved) ?? .full
        secondTimeZone = SecondTimeZone(
            rawValue: UserDefaults.standard.string(forKey: Self.secondTimeZoneKey) ?? "",
        ) ?? .off

        NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.didWakeNotification, object: nil, queue: .main,
        ) { [weak self] _ in
            Task { @MainActor in self?.restart() }
        }
    }

    private func restart() {
        guard samplingActive else { return }
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
        var text = formatter.string(from: Date())
        if let timeZone = secondTimeZone.timeZone {
            secondFormatter.timeZone = timeZone
            text += " \(secondTimeZone.code) \(secondFormatter.string(from: Date()))"
        }
        labelText = text
    }
}
