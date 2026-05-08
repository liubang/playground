import "array"
import "csv"
import "sqlite"

cpu = sqlite.from(
    path: "cpp/pl/flux/examples/cross_source/metrics.db",
    table: "cpu",
)
    |> range(start: 2024-07-01T10:00:00Z, stop: 2024-07-01T10:04:00Z)
    |> filter(fn: (r) => r.region == "west")
    |> group(columns: ["host"])
    |> mean(column: "usage")
    |> keep(columns: ["host", "usage"])

owners = csv.from(file: "cpp/pl/flux/examples/cross_source/owners.csv", mode: "raw")
    |> filter(fn: (r) => r.tier == "prod")
    |> group(columns: ["host"])

limits = array.from(rows: [
    {host: "edge-1", limit: 80.0, severity: "page"},
    {host: "edge-2", limit: 85.0, severity: "ticket"},
    {host: "edge-3", limit: 90.0, severity: "watch"},
])
    |> group(columns: ["host"])

owned = join(tables: {cpu: cpu, owners: owners}, on: ["host"])
    |> group(columns: ["host"])

join(tables: {owned: owned, limits: limits}, on: ["host"])
    |> filter(fn: (r) => r.usage >= r.limit)
    |> keep(columns: ["host", "owner", "tier", "usage", "limit", "severity"])
    |> sort(columns: ["usage"], desc: true)
    |> yield(name: "sqlite_csv_array_incidents")
