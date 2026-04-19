from(
    bucket: "telegraf",
    rows: [
        {_time: "2024-01-20T00:00:00Z", _value: 10.0},
        {_time: "2024-02-20T00:00:00Z", _value: 30.0},
    ],
)
    |> aggregateWindow(every: 1mo, offset: 15d, fn: mean, createEmpty: false)
    |> yield(name: "monthly_cpu_calendar_offset")
