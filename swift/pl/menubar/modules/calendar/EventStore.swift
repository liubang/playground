import EventKit
import Foundation

/// Wraps EKEventStore: authorization flow plus event lookups for the
/// popover — a single day's events for the detail list, and a month-grid
/// range fetch for the per-cell dot markers.
///
/// Predicate queries go to the calendar daemon and always return live
/// data; `revision` simply re-triggers cell rebuilds after the store
/// reports external changes.
@MainActor
final class EventStore: ObservableObject {
    enum Status: Equatable {
        case notDetermined
        case denied
        case fullAccess
    }

    @Published private(set) var status: Status
    /// Bumped when events change externally or access is granted —
    /// observed by the popover to rebuild the grid.
    @Published private(set) var revision = 0

    private let store = EKEventStore()

    init() {
        status = Self.mapStatus(EKEventStore.authorizationStatus(for: .event))
        NotificationCenter.default.addObserver(
            forName: .EKEventStoreChanged, object: store, queue: .main,
        ) { [weak self] _ in
            Task { @MainActor in self?.revision += 1 }
        }
    }

    private static func mapStatus(_ value: EKAuthorizationStatus) -> Status {
        switch value {
        case .notDetermined: .notDetermined
        case .fullAccess: .fullAccess
        default: .denied
        }
    }

    /// Ask for read access. Only meaningful from .notDetermined; the
    /// system shows its prompt at most once.
    func requestAccess() {
        guard status == .notDetermined else { return }
        store.requestFullAccessToEvents { [weak self] granted, _ in
            Task { @MainActor in
                guard let self else { return }
                self.status = granted ? .fullAccess : .denied
                self.revision += 1
            }
        }
    }

    /// Events intersecting [start, end).
    func events(from start: Date, to end: Date) -> [EKEvent] {
        guard status == .fullAccess else { return [] }
        let predicate = store.predicateForEvents(withStart: start, end: end, calendars: nil)
        return store.events(matching: predicate)
    }

    /// Events on a single day: all-day first, then by start time.
    func events(on day: Date) -> [EKEvent] {
        let cal = CalendarModel.calendar
        let start = cal.startOfDay(for: day)
        guard let end = cal.date(byAdding: .day, value: 1, to: start) else { return [] }
        return events(from: start, to: end).sorted {
            if $0.isAllDay != $1.isAllDay {
                return $0.isAllDay
            }
            return $0.startDate < $1.startDate
        }
    }

    /// Local start-of-day dates within the range having at least one
    /// event — the grid's dot markers. Event end dates are exclusive
    /// (an all-day event "on Friday" ends Saturday 00:00), so the last
    /// covered day is derived from one second before the end.
    func eventDays(from start: Date, to end: Date) -> Set<Date> {
        let cal = CalendarModel.calendar
        var days = Set<Date>()
        for event in events(from: start, to: end) {
            var day = cal.startOfDay(for: event.startDate)
            let last = cal.startOfDay(for: event.endDate.addingTimeInterval(-1))
            while day <= last {
                days.insert(day)
                guard let next = cal.date(byAdding: .day, value: 1, to: day) else { break }
                day = next
            }
        }
        return days
    }
}
