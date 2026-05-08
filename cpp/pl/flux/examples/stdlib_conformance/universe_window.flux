import "array"

data = array.from(
    bucket: "window",
    rows: [
        {_time: 2024-01-01T00:00:00Z, host: "edge-1", _value: 10.0},
        {_time: 2024-01-01T00:01:00Z, host: "edge-1", _value: 15.0},
        {_time: 2024-01-01T00:02:00Z, host: "edge-1", _value: 25.0},
    ],
)

elapsed_row = data
    |> elapsed(unit: 1m)
    |> findRecord(fn: (r) => r.host == "edge-1", idx: 0)

difference_row = data
    |> difference()
    |> findRecord(fn: (r) => r.host == "edge-1", idx: 0)

derivative_row = data
    |> derivative(unit: 1m)
    |> findRecord(fn: (r) => r.host == "edge-1", idx: 0)

windowed_counts = data
    |> window(every: 2m, createEmpty: true)
    |> count()
    |> findColumn(fn: (r) => true, column: "_value")

aggregated = data
    |> aggregateWindow(every: 2m, fn: mean, createEmpty: true)
    |> findRecord(fn: (r) => true, idx: 0)

{
    elapsed: elapsed_row.elapsed,
    difference: difference_row._value,
    derivative: derivative_row._value,
    window_counts: windowed_counts,
    aggregate_mean: aggregated._value,
}
