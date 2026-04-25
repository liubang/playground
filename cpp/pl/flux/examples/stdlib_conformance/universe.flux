data = from(
    bucket: "conformance",
    rows: [
        {_time: 2024-01-01T00:00:00Z, host: "edge-1", _value: 10.0},
        {_time: 2024-01-01T00:01:00Z, host: "edge-1", _value: 15.0},
        {_time: 2024-01-01T00:02:00Z, host: "edge-2", _value: 20.0},
    ],
)

row = data
    |> range(start: 2024-01-01T00:00:00Z, stop: 2024-01-01T00:03:00Z)
    |> filter(fn: (r) => r.host == "edge-1")
    |> group(columns: ["host"])
    |> count(column: "_value")
    |> findRecord(fn: (r) => r.host == "edge-1", idx: 0)

{
    host: row.host,
    count: row._value,
}

