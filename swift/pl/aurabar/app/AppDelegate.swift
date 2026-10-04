import AppKit
import SwiftUI

/// Wires up the modules at launch: stores, status item controllers and
/// label bindings. Adding a module = one store + one `makeModule` call
/// + one label binding here.
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private var calendarController: StatusItemController?
    private var weatherController: StatusItemController?
    private var cpuController: StatusItemController?
    private var memoryController: StatusItemController?
    private var networkController: StatusItemController?
    private var gpuController: StatusItemController?
    private var diskController: StatusItemController?
    private var batteryController: StatusItemController?

    /// One visibility-driven module = one controller + both visibility
    /// feeds wired to its store. The label binding stays at the call
    /// site — it differs per module.
    private func makeModule(
        autosaveName: String,
        visibilityKey: String,
        store: some VisibilityDriven,
        content: some View,
    ) -> StatusItemController {
        let item = StatusItemController(
            autosaveName: autosaveName,
            visibilityKey: visibilityKey,
            content: content,
        )
        item.observeStatusItemVisibility { [weak store] visible in
            store?.statusItemVisibilityChanged(visible)
        }
        item.onPopoverVisibilityChange = { [weak store] open in
            store?.popoverVisibilityChanged(open)
        }
        return item
    }

    func applicationDidFinishLaunching(_: Notification) {
        _ = AppNapDisabler.shared

        // Calendar: glyph + clock text, ticking on the minute boundary.
        // EventStore feeds system-calendar events into the popover;
        // HolidaySync keeps the statutory-holiday table fresh remotely.
        let clock = MenuBarClock()
        AppRegistry.clock = clock
        let eventStore = EventStore()
        let holidaySync = HolidaySync()
        let calendar = StatusItemController(
            autosaveName: calendarModuleAutosaveName,
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
        calendar.observeStatusItemVisibility { [weak clock] visible in
            clock?.setActive(visible)
        }
        calendarController = calendar

        // Weather: condition symbol + temperature.
        let weather = WeatherStore()
        AppRegistry.weather = weather
        let weatherItem = makeModule(
            autosaveName: ModuleVisibility.weatherAutosave,
            visibilityKey: ModuleVisibility.weatherKey,
            store: weather,
            content: WeatherPopover(store: weather),
        )
        weatherItem.bindLabel(to: weather.$snapshot) { button, snapshot in
            guard let button else { return }
            button.imagePosition = .imageLeading
            button.font = .monospacedDigitSystemFont(
                ofSize: NSFont.systemFontSize,
                weight: .regular,
            )
            // Rendered into the shared 16pt box + edge margin the stats
            // glyphs use, so it matches their optical size and spacing.
            button.image = StatsGlyphs.makeSymbol(
                snapshot?.current.condition.symbolName ?? "cloud",
            )
            button.title = snapshot?.displayTemperature ?? "--°"
            button.toolTip = snapshot.map {
                "\($0.location.name) · \($0.current.condition.label) \($0.displayTemperature)"
            }
        }
        weatherController = weatherItem

        // Stats: one shared sampler feeding three status items — CPU
        // (donut gauge + CPU/percent label), memory (level gauge +
        // MEM/used label), network (arrow pair + up/down lines). The
        // label text is drawn into the item's image, Stats-widget style.
        let stats = SystemStatsStore()

        let cpuItem = makeModule(
            autosaveName: ModuleVisibility.cpuAutosave,
            visibilityKey: ModuleVisibility.cpuKey,
            store: stats,
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
        cpuController = cpuItem

        let memoryItem = makeModule(
            autosaveName: ModuleVisibility.memoryAutosave,
            visibilityKey: ModuleVisibility.memoryKey,
            store: stats,
            content: MemoryPopover(store: stats),
        )
        memoryItem.bindLabel(to: stats.$memoryUsed.combineLatest(stats.$memoryTotal)) { button, pair in
            guard let button else { return }
            let (used, total) = pair
            button.image = StatsGlyphs.makeMemory(
                fraction: Double(used) / Double(max(total, 1)),
                value: Formatters.bytes(used),
            )
            button.imagePosition = .imageOnly
            button.title = ""
            button.toolTip = "内存：\(Formatters.usagePair(used, max(total, 1)))"
        }
        memoryController = memoryItem

        let networkItem = makeModule(
            autosaveName: ModuleVisibility.networkAutosave,
            visibilityKey: ModuleVisibility.networkKey,
            store: stats,
            content: NetworkPopover(store: stats),
        )
        networkItem.bindLabel(to: stats.$downRate.combineLatest(stats.$upRate)) { button, rates in
            guard let button else { return }
            let (down, up) = rates
            button.image = StatsGlyphs.makeNetwork(up: up, down: down)
            button.imagePosition = .imageOnly
            button.title = ""
            button.toolTip = "\u{2191}\(Formatters.rate(up))/s \u{2193}\(Formatters.rate(down))/s"
        }
        networkController = networkItem

        // GPU: fan glyph + percentage, driven by its own IOKit sampler
        // (unlike the other stats items the driver read is a single
        // property fetch, so it doesn't join SystemStatsStore).
        let gpu = GPUStore()
        let gpuItem = makeModule(
            autosaveName: ModuleVisibility.gpuAutosave,
            visibilityKey: ModuleVisibility.gpuKey,
            store: gpu,
            content: GPUPopover(store: gpu),
        )
        gpuItem.bindLabel(to: gpu.$usage) { button, usage in
            guard let button else { return }
            button.image = StatsGlyphs.makeGPU(
                fraction: usage,
                value: "\(Int((usage * 100).rounded()))%",
            )
            button.imagePosition = .imageOnly
            button.title = ""
            button.toolTip = "GPU：\(Int((usage * 100).rounded()))%"
        }
        gpuController = gpuItem

        // Disk: drive icon + write/read rate lines, diffed from the
        // block-storage drivers' cumulative byte counters.
        let disk = DiskStore()
        let diskItem = makeModule(
            autosaveName: ModuleVisibility.diskAutosave,
            visibilityKey: ModuleVisibility.diskKey,
            store: disk,
            content: DiskPopover(store: disk),
        )
        diskItem.bindLabel(to: disk.$readRate.combineLatest(disk.$writeRate)) { button, rates in
            guard let button else { return }
            let (read, write) = rates
            button.image = StatsGlyphs.makeDisk(read: read, write: write)
            button.imagePosition = .imageOnly
            button.title = ""
            button.toolTip = "写入 \(Formatters.rate(write))/s · 读取 \(Formatters.rate(read))/s"
        }
        diskController = diskItem

        // Battery: level glyph + percentage, event-driven via IOKit
        // power-source notifications (30s timer as fallback). The module
        // is skipped entirely on machines without an internal battery
        // (Mac mini, Mac Studio) instead of showing a meaningless "--".
        let battery = BatteryStore()
        AppRegistry.hasBattery = battery.info != nil
        if battery.info != nil {
            let batteryItem = makeModule(
                autosaveName: ModuleVisibility.batteryAutosave,
                visibilityKey: ModuleVisibility.batteryKey,
                store: battery,
                content: BatteryPopover(store: battery),
            )
            batteryItem.bindLabel(to: battery.$info) { button, info in
                guard let button else { return }
                button.image = StatsGlyphs.makeBattery(
                    fraction: Double(info?.percentage ?? 0) / 100,
                    charging: info?.isCharging ?? false,
                    value: info.map { "\($0.percentage)%" } ?? "--",
                )
                button.imagePosition = .imageOnly
                button.title = ""
                button.toolTip = info.map { "电池：\($0.percentage)%" }
            }
            batteryController = batteryItem
        }
    }
}
