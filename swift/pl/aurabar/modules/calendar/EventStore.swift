import EventKit
import Foundation

/// Wraps EKEventStore: authorization flow plus event lookups for the
/// popover — a single day's events for the detail list, and a month-grid
/// range fetch for the per-cell dot markers.
///
/// Predicate queries go to the calendar daemon and always return live
/// data; `revision` simply re-triggers cell rebuilds after the store
/// reports external changes.
///
/// Threading: every touch of the underlying EKEventStore funnels through
/// a serial queue, so the popover can run its (potentially slow) daemon
/// round-trips off the main thread inside detached rebuild tasks.
/// Published state (`status`, `revision`) is only written on the main
/// actor.
final class EventStore: ObservableObject, @unchecked Sendable {
    enum Status: Equatable {
        case notDetermined
        case denied
        case fullAccess
    }

    @Published private(set) var status: Status
    /// Bumped when events change externally or access is granted —
    /// observed by the popover to rebuild the grid. Main-actor writes.
    @Published private(set) var revision = 0

    private let store = EKEventStore()
    /// Serializes every EKEventStore access plus the mutable query state
    /// (revision counter, day cache). Never call `store` outside `queue`.
    private let queue = DispatchQueue(label: "aurabar.eventstore")

    /// Queue-guarded mutable state mirrored by the published properties.
    private struct State {
        var revision = 0
        /// Single-day lookup cache: the popover re-asks on every
        /// selection change, but events only change with `revision` or
        /// the selected day.
        var dayCache: (day: Date, revision: Int, events: [EKEvent])?
    }

    /// Only mutated inside `queue`.
    private var state = State()
    /// Coalescing flag for EKEventStoreChanged bursts — main-actor only.
    @MainActor private var bumpPending = false

    init() {
        status = Self.mapStatus(EKEventStore.authorizationStatus(for: .event))
        NotificationCenter.default.addObserver(
            forName: .EKEventStoreChanged, object: store, queue: nil,
        ) { [weak self] _ in
            self?.noteExternalChange()
        }
    }

    private static func mapStatus(_ value: EKAuthorizationStatus) -> Status {
        switch value {
        case .notDetermined: .notDetermined
        case .fullAccess: .fullAccess
        default: .denied
        }
    }

    // MARK: - Revision bumps

    /// EKEventStoreChanged arrives in bursts when the calendar daemon
    /// applies a batch; coalesce to at most one bump per window so the
    /// popover doesn't spin full grid rebuilds back-to-back.
    private func noteExternalChange() {
        Task { @MainActor [weak self] in
            guard let self, !self.bumpPending else { return }
            bumpPending = true
            try? await Task.sleep(nanoseconds: 700_000_000)
            bumpPending = false
            bumpRevision()
        }
    }

    /// Raises the queue-guarded counter and republishes it on main.
    @MainActor
    private func bumpRevision() {
        let value = queue.sync {
            state.revision += 1
            return state.revision
        }
        revision = value
    }

    // MARK: - Authorization

    /// Ask for read access. Only meaningful from .notDetermined; the
    /// system shows its prompt at most once.
    func requestAccess() {
        guard status == .notDetermined else { return }
        queue.async { [weak self] in
            self?.store.requestFullAccessToEvents { [weak self] granted, _ in
                Task { @MainActor in
                    guard let self else { return }
                    self.status = granted ? .fullAccess : .denied
                    self.bumpRevision()
                }
            }
        }
    }

    // MARK: - Queries

    /// Events intersecting [start, end). Callable from any thread.
    func events(from start: Date, to end: Date) -> [EKEvent] {
        guard status == .fullAccess else { return [] }
        return queue.sync {
            let predicate = store.predicateForEvents(withStart: start, end: end, calendars: nil)
            return store.events(matching: predicate)
        }
    }

    /// Events on a single day: all-day first, then by start time.
    /// Callable from any thread.
    func events(on day: Date) -> [EKEvent] {
        let cal = CalendarModel.calendar
        let start = cal.startOfDay(for: day)
        return queue.sync {
            if let cache = state.dayCache, cache.day == start, cache.revision == state.revision {
                return cache.events
            }
            guard status == .fullAccess,
                  let end = cal.date(byAdding: .day, value: 1, to: start) else { return [] }
            let predicate = store.predicateForEvents(withStart: start, end: end, calendars: nil)
            let sorted = store.events(matching: predicate).sorted {
                if $0.isAllDay != $1.isAllDay {
                    return $0.isAllDay
                }
                return $0.startDate < $1.startDate
            }
            state.dayCache = (start, state.revision, sorted)
            return sorted
        }
    }

    /// Local start-of-day dates covered by [startDate, endDate]. Event
    /// end dates are exclusive (an all-day event "on Friday" ends
    /// Saturday 00:00), so the last covered day is derived from one
    /// second before the end.
    static func daysSpanned(from startDate: Date, to endDate: Date) -> Set<Date> {
        let cal = CalendarModel.calendar
        var days = Set<Date>()
        var day = cal.startOfDay(for: startDate)
        let last = cal.startOfDay(for: endDate.addingTimeInterval(-1))
        while day <= last {
            days.insert(day)
            guard let next = cal.date(byAdding: .day, value: 1, to: day) else { break }
            day = next
        }
        return days
    }

    /// Local start-of-day dates within the range having at least one
    /// event — the grid's dot markers. Callable from any thread.
    func eventDays(from start: Date, to end: Date) -> Set<Date> {
        var days = Set<Date>()
        for event in events(from: start, to: end) {
            days.formUnion(Self.daysSpanned(from: event.startDate, to: event.endDate))
        }
        return days
    }
}
