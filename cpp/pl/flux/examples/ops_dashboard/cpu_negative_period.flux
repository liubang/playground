from(
    bucket: "telegraf",
    rows: [
        {_time: "2024-01-01T00:00:00Z", _value: 2.0},
        {_time: "2024-01-01T00:00:10Z", _value: 4.0},
        {_time: "2024-01-01T00:00:20Z", _value: 6.0},
        {_time: "2024-01-01T00:00:30Z", _value: 8.0},
        {_time: "2024-01-01T00:00:40Z", _value: 10.0},
        {_time: "2024-01-01T00:00:50Z", _value: 12.0},
    ],
)
    |> range(start: 2024-01-01T00:00:00Z, stop: 2024-01-01T00:01:00Z)
    |> aggregateWindow(every: 20s, period: "-40s", fn: count, createEmpty: false)
    |> yield(name: "cpu_negative_period")
