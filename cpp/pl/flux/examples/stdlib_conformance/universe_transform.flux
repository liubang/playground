base = from(
    bucket: "transform",
    rows: [
        {_time: 2024-01-01T00:00:00Z, host: "edge-1", service: "api", _field: "cpu", _value: 10.0},
        {_time: 2024-01-01T00:01:00Z, host: "edge-1", service: "api", _field: "mem", _value: 20.0},
        {_time: 2024-01-01T00:02:00Z, host: "edge-2", service: "db", _field: "cpu", _value: 30.0},
    ],
)

shaped = base
    |> range(start: 2024-01-01T00:00:00Z, stop: 2024-01-01T00:03:00Z)
    |> filter(fn: (r) => r._field == "cpu")
    |> map(fn: (r) => ({r with score: r._value + 1.0}))
    |> duplicate(column: "_value", as: "raw")
    |> set(key: "env", value: "prod")
    |> rename(columns: {_value: "value"})
    |> drop(columns: ["raw"])
    |> keep(columns: ["_time", "host", "service", "value", "score", "env"])
    |> sort(columns: ["value"], desc: true)
    |> limit(n: 2)
    |> tail(n: 1)
    |> group(columns: ["service"])

pivoted = base
    |> pivot(rowKey: ["host"], columnKey: ["_field"], valueColumn: "_value")
    |> findRecord(fn: (r) => r.host == "edge-1", idx: 0)

filled = base
    |> filter(fn: (r) => r.host == "missing", onEmpty: "keep")
    |> fill(column: "_value", value: 0.0)

unioned = union(tables: [shaped, filled])

row = shaped |> findRecord(fn: (r) => r.host == "edge-1", idx: 0)
unioned_count = unioned
    |> count(column: "host")
    |> findRecord(fn: (r) => exists r.host, idx: 0)

{
    row_host: row.host,
    row_service: row.service,
    row_value: row.value,
    row_score: row.score,
    row_env: row.env,
    pivot_cpu: pivoted.cpu,
    pivot_mem: pivoted.mem,
    union_rows: unioned_count.host,
}
