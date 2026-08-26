import AppKit

/// Wires up the modules at launch: stores, status item controllers and
/// label bindings. Adding a module = one store + one controller here.
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private var calendarController: StatusItemController?
    private var weatherController: StatusItemController?
    private var cpuController: StatusItemController?
    private var memoryController: StatusItemController?
    private var networkController: StatusItemController?

    func applicationDidFinishLaunching(_: Notification) {
        _ = AppNapDisabler.shared

        // Calendar: glyph + clock text, ticking on the minute boundary.
        // EventStore feeds system-calendar events into the popover;
        // HolidaySync keeps the statutory-holiday table fresh remotely.
        let clock = MenuBarClock()
        let eventStore = EventStore()
        let holidaySync = HolidaySync()
        let calendar = StatusItemController(
            autosaveName: "AuraBar.calendar",
            visibilityKey: ModuleVisibility.calendarKey,
            content: CalendarPopover(clock: clock, eventStore: eventStore, holidaySync: holidaySync),
        )
        calendar.bindLabel(to: clock.$labelText) { button, text in
            guard let button else { return }
            if button.image == nil {
                button.image = MenuBarGlyph.make()
                button.imagePosition = .imageLeading
                button.font = .monospacedDigitSystemFont(
                    ofSize: NSFont.systemFontSize,
                    weight: .regular,
                )
            }
            button.title = text
            button.toolTip = Date.now.formatted(date: .complete, time: .omitted)
        }
        calendarController = calendar

        // Weather: condition symbol + temperature.
        let weather = WeatherStore()
        let weatherItem = StatusItemController(
            autosaveName: "AuraBar.weather",
            visibilityKey: ModuleVisibility.weatherKey,
            content: WeatherPopover(store: weather),
        )
        weatherItem.bindLabel(to: weather.$snapshot) { button, snapshot in
            guard let button else { return }
            button.imagePosition = .imageLeading
            button.font = .monospacedDigitSystemFont(
                ofSize: NSFont.systemFontSize,
                weight: .regular,
            )
            let image = NSImage(
                systemSymbolName: snapshot?.current.condition.symbolName ?? "cloud",
                accessibilityDescription: nil,
            )
            image?.isTemplate = true
            button.image = image
            button.title = snapshot.map { "\(Int($0.current.temperature.rounded()))°" } ?? "--°"
            button.toolTip = snapshot.map {
                "\($0.location.name) · \($0.current.condition.label) \(Int($0.current.temperature.rounded()))°"
            }
        }
        weatherController = weatherItem

        // Stats: one shared sampler feeding three status items — CPU
        // (donut gauge + CPU/percent label), memory (level gauge +
        // MEM/used label), network (arrow pair + up/down lines). The
        // label text is drawn into the item's image, Stats-widget style.
        let stats = SystemStatsStore()

        let cpuItem = StatusItemController(
            autosaveName: "AuraBar.cpu",
            visibilityKey: ModuleVisibility.cpuKey,
            content: CPUPopover(store: stats),
        )
        cpuItem.bindLabel(to: stats.$cpuUsage) { button, usage in
            guard let button else { return }
            button.image = StatsGlyphs.makeCPU(
                fraction: usage,
                value: "\(Int((usage * 100).rounded()))%",
            )
            button.imagePosition = .imageOnly
            button.title = ""
            button.toolTip = "CPU：\(Int((usage * 100).rounded()))%"
        }
        cpuItem.onVisibilityChange = { [weak stats] open in
            open ? stats?.popoverDidOpen() : stats?.popoverDidClose()
        }
        cpuController = cpuItem

        let memoryItem = StatusItemController(
            autosaveName: "AuraBar.memory",
            visibilityKey: ModuleVisibility.memoryKey,
            content: MemoryPopover(store: stats),
        )
        memoryItem.bindLabel(to: stats.$memoryUsed) { [weak stats] button, used in
            guard let button else { return }
            let total = max(stats?.memoryTotal ?? 1, 1)
            button.image = StatsGlyphs.makeMemory(
                fraction: Double(used) / Double(total),
                value: Formatters.bytes(used),
            )
            button.imagePosition = .imageOnly
            button.title = ""
            button.toolTip = "内存：\(Formatters.usagePair(used, total))"
        }
        memoryItem.onVisibilityChange = { [weak stats] open in
            open ? stats?.popoverDidOpen() : stats?.popoverDidClose()
        }
        memoryController = memoryItem

        let networkItem = StatusItemController(
            autosaveName: "AuraBar.network",
            visibilityKey: ModuleVisibility.networkKey,
            content: NetworkPopover(store: stats),
        )
        networkItem.bindLabel(to: stats.$downRate) { [weak stats] button, down in
            guard let button, let stats else { return }
            let up = stats.upRate
            button.image = StatsGlyphs.makeNetwork(up: up, down: down)
            button.imagePosition = .imageOnly
            button.title = ""
            button.toolTip = "\u{2191}\(Formatters.rate(up))/s \u{2193}\(Formatters.rate(down))/s"
        }
        networkItem.onVisibilityChange = { [weak stats] open in
            open ? stats?.popoverDidOpen() : stats?.popoverDidClose()
        }
        networkController = networkItem
    }
}
