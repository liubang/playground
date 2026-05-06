import "sql"

row = sql.from(
    driver: "sqlite",
    dsn: ":memory:",
    query: "select '2024-01-01T00:00:00Z' as _time, 'edge-1' as host, 91.5 as _value, null as note union all select '2024-01-01T00:01:00Z', 'edge-2', 42.0, 'ok'",
)
    |> filter(fn: (r) => r.host == "edge-1")
    |> findRecord(fn: (r) => r.host == "edge-1", idx: 0)

{
    host: row.host,
    value: row._value,
    note: row.note,
}
