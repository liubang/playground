import "array"
import "csv"
import "mysql"
import "sqlite"

mysqlHost = "127.0.0.1"
mysqlPort = 3306
mysqlUser = "flux"
mysqlPassword = "flux"
mysqlDatabase = "flux_test"

mysqlCpu = mysql.from(
    host: mysqlHost,
    port: mysqlPort,
    user: mysqlUser,
    password: mysqlPassword,
    database: mysqlDatabase,
    table: "cpu",
)
    |> range(start: 2024-07-01T10:00:00Z, stop: 2024-07-01T10:05:00Z)
    |> filter(fn: (r) => r.region == "west")
    |> group(columns: ["host"])
    |> mean(column: "usage")
    |> keep(columns: ["host", "usage"])

sqliteCpu = sqlite.from(
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
    {host: "edge-2", limit: 78.0, severity: "ticket"},
    {host: "edge-3", limit: 90.0, severity: "watch"},
])
    |> group(columns: ["host"])

paired = join(tables: {mysql: mysqlCpu, sqlite: sqliteCpu}, on: ["host"])
    |> group(columns: ["host"])

owned = join(tables: {paired: paired, owners: owners}, on: ["host"])
    |> group(columns: ["host"])

join(tables: {owned: owned, limits: limits}, on: ["host"])
    |> filter(fn: (r) => r.usage_mysql >= r.limit)
    |> keep(columns: ["host", "owner", "tier", "usage_mysql", "usage_sqlite", "limit", "severity"])
    |> sort(columns: ["usage_mysql"], desc: true)
    |> yield(name: "mysql_sqlite_csv_array_incidents")
