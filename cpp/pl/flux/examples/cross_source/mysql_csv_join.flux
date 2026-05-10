import "csv"
import "mysql"

mysqlHost = "127.0.0.1"
mysqlPort = 3306
mysqlUser = "flux"
mysqlPassword = "flux"
mysqlDatabase = "flux_test"

cpu = mysql.from(
    host: mysqlHost,
    port: mysqlPort,
    user: mysqlUser,
    password: mysqlPassword,
    database: mysqlDatabase,
    table: "cpu",
)
    |> range(start: 2024-07-01T10:00:30Z, stop: 2024-07-01T10:05:00Z)
    |> keep(columns: ["_time", "host", "region", "usage", "active"])
    |> filter(fn: (r) => r.active == true)
    |> sort(columns: ["usage"], desc: true)
    |> limit(n: 3)
    |> group(columns: ["host"])

owners = csv.from(file: "cpp/pl/flux/examples/cross_source/owners.csv", mode: "raw")
    |> group(columns: ["host"])

join(tables: {cpu: cpu, owners: owners}, on: ["host"])
    |> keep(columns: ["host", "region", "_time", "usage", "owner", "tier"])
    |> sort(columns: ["usage"], desc: true)
    |> yield(name: "mysql_csv_join")
