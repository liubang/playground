import "csv"
import "datasource"

cpu = datasource.from(
    driver: "sqlite",
    dsn: "cpp/pl/flux/examples/cross_source/metrics.db",
    table: "cpu",
)
    |> range(start: 2024-07-01T10:00:30Z, stop: 2024-07-01T10:04:00Z)
    |> keep(columns: ["_time", "host", "region", "usage"])
    |> sort(columns: ["usage"], desc: true)
    |> limit(n: 2)
    |> group(columns: ["host"])

owners = csv.from(file: "cpp/pl/flux/examples/cross_source/owners.csv", mode: "raw")
    |> group(columns: ["host"])

join(tables: {cpu: cpu, owners: owners}, on: ["host"])
    |> keep(columns: ["host", "region", "_time", "usage", "owner", "tier"])
    |> sort(columns: ["usage"], desc: true)
    |> yield(name: "sqlite_csv_join")
