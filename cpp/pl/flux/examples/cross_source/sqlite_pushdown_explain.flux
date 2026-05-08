import "sql"

data = sql.from(
    driver: "sqlite",
    dsn: "cpp/pl/flux/examples/cross_source/metrics.db",
    table: "cpu",
)
    |> range(start: 2024-07-01T10:00:00Z, stop: 2024-07-01T10:04:00Z)
    |> filter(fn: (r) => r.host == "edge-1")
    |> filter(fn: (r) => r.usage > 80.0)
    |> drop(columns: ["region"])
    |> rename(columns: {usage: "value"})
    |> keep(columns: ["_time", "host", "value"])
    |> sort(columns: ["value"], desc: true)
    |> limit(n: 10)

services = sql.from(
    driver: "sqlite",
    dsn: "cpp/pl/flux/examples/cross_source/metrics.db",
    table: "cpu",
)
    |> rename(columns: {host: "service"})
    |> distinct(column: "service")
    |> keep(columns: ["service"])
    |> sort(columns: ["service"], desc: false)

{
    values: data |> findColumn(fn: (r) => true, column: "value"),
    services: services |> findColumn(fn: (r) => true, column: "service"),
    plan: data |> explain(),
    distinctPlan: services |> explain(),
}
