import "csv"
import "sql"

cpu = sql.from(
    driver: "sqlite",
    dsn: "cpp/pl/flux/examples/cross_source/metrics.db",
    query: "select _time, host, region, usage as _value from cpu",
)
    |> range(start: 2024-07-01T10:00:30Z, stop: 2024-07-01T10:04:00Z)
    |> keep(columns: ["_time", "host", "region", "_value"])
    |> sort(columns: ["_value"], desc: true)
    |> limit(n: 2)
    |> group(columns: ["host"])

owners = csv.from(file: "cpp/pl/flux/examples/cross_source/owners.csv", mode: "raw")
    |> group(columns: ["host"])

join(tables: {cpu: cpu, owners: owners}, on: ["host"])
    |> keep(columns: ["host", "region", "_time", "_value", "owner", "tier"])
    |> sort(columns: ["_value"], desc: true)
    |> yield(name: "sqlite_csv_join")
