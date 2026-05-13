import "mysql"

mysqlHost = "127.0.0.1"
mysqlPort = 3306
mysqlUser = "flux"
mysqlPassword = "flux"
mysqlDatabase = "flux_test"

data = mysql.from(
    host: mysqlHost,
    port: mysqlPort,
    user: mysqlUser,
    password: mysqlPassword,
    database: mysqlDatabase,
    table: "cpu",
)
    |> range(start: 2024-07-01T10:00:00Z, stop: 2024-07-01T10:04:00Z)
    |> filter(fn: (r) => r.host == "edge-1")
    |> filter(fn: (r) => r.usage > 80.0)
    |> drop(columns: ["region", "cores", "active", "note"])
    |> rename(columns: {usage: "value"})
    |> keep(columns: ["_time", "host", "value"])
    |> sort(columns: ["value"], desc: true)
    |> limit(n: 10)

services = mysql.from(
    host: mysqlHost,
    port: mysqlPort,
    user: mysqlUser,
    password: mysqlPassword,
    database: mysqlDatabase,
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
    physicalPlan: data |> explain(physical: true),
    distinctPlan: services |> explain(),
    distinctPhysicalPlan: services |> explain(physical: true),
}
