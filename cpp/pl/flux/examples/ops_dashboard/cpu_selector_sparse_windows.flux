from(
    bucket: "telegraf",
    rows: [
        {_time: "2024-01-01T00:00:10Z", _value: 10.0},
        {_time: "2024-01-01T00:02:05Z", _value: 30.0},
    ],
)
    |> range(start: 2024-01-01T00:00:00Z, stop: 2024-01-01T00:03:00Z)
    |> aggregateWindow(every: 1m, fn: last, createEmpty: true)
    |> yield(name: "cpu_selector_sparse_windows")
