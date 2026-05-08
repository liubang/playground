import "datasource"

row = datasource.from(
    driver: "sqlite",
    dsn: "cpp/pl/flux/examples/cross_source/metrics.db",
    table: "cpu",
)
    |> filter(fn: (r) => r.host == "edge-1")
    |> findRecord(fn: (r) => r.host == "edge-1", idx: 0)

{
    host: row.host,
    value: row.usage,
    region: row.region,
}
