import "array"
import "timezone"

fixed = timezone.fixed(offset: -8h)
named = timezone.location(name: "America/Los_Angeles")
window_row = array.from(
        rows: [
            {_time: 2024-01-01T00:15:00Z, _value: 1.0},
            {_time: 2024-01-01T08:15:00Z, _value: 3.0},
        ],
    )
    |> aggregateWindow(every: 1d, fn: mean, location: fixed)
    |> findRecord(fn: (r) => true, idx: 0)

{
    utc_zone: timezone.utc.zone,
    utc_offset: timezone.utc.offset,
    fixed_zone: fixed.zone,
    fixed_offset: fixed.offset,
    named_zone: named.zone,
    named_offset: named.offset,
    fixed_window_start: window_row._start,
}
