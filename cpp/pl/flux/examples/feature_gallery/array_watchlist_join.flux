import "array"
import "csv"

baseWatchlist = ["edge-1", "edge-2"]
    |> array.map(fn: (host) => ({host: host, owner: "ops", cohort: "primary"}))

watchlist = baseWatchlist
    |> array.concat(v: [{host: "edge-3", owner: "ops", cohort: "canary"}])
    |> array.filter(fn: (r) => r.host != "edge-2")

watchHosts = watchlist |> array.map(fn: (r) => r.host)
hasCanary = watchHosts |> array.contains(value: "edge-3")
watchSummary = watchHosts
    |> array.reduce(
        identity: {count: 0, last: ""},
        fn: (host, accumulator) => ({
            count: accumulator.count + 1,
            last: host,
        }),
    )
hasPrimaryOwner = watchlist |> array.any(fn: (r) => r.owner == "ops")
allNamedHosts = watchHosts |> array.all(fn: (host) => host != "")

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
    |> set(key: "watchlist_last", value: watchSummary.last)
    |> set(key: "watchlist_count", value: watchSummary.count)
    |> set(key: "has_canary", value: hasCanary)
    |> set(key: "has_primary_owner", value: hasPrimaryOwner)
    |> set(key: "all_named_hosts", value: allNamedHosts)
    |> yield(name: "array_watchlist_join")
