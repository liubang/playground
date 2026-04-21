import "array"
import "csv"

baseWatchlist = ["edge-1", "edge-2"]
    |> array.map(fn: (host) => ({host: host, owner: "ops", cohort: "primary"}))

watchlist = baseWatchlist
    |> array.concat(v: [{host: "edge-3", owner: "ops", cohort: "canary"}])
    |> array.filter(fn: (r) => r.host != "edge-2")

inventory = array.from(rows: watchlist)
    |> group(columns: ["host"])

cpu = csv.from(file: "cpp/pl/flux/examples/feature_gallery/data/site_ops.annotated.csv")
    |> filter(fn: (r) => r._measurement == "cpu" and r._field == "usage_user")
    |> aggregateWindow(every: 1m, fn: mean, createEmpty: false)
    |> group(columns: ["host"])

join(
    tables: {inventory: inventory, cpu: cpu},
    method: "inner",
    on: ["host"],
)
    |> yield(name: "array_watchlist_join")
