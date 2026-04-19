import "csv"

cpu = csv.from(file: "cpp/pl/flux/examples/feature_gallery/site_ops.annotated.csv")
    |> filter(fn: (r) => r._measurement == "cpu")

mem = csv.from(file: "cpp/pl/flux/examples/feature_gallery/site_ops.annotated.csv")
    |> filter(fn: (r) => r._measurement == "mem")

join(
    tables: {
        cpu: cpu
            |> filter(fn: (r) => r.host == "edge-1")
            |> aggregateWindow(every: 1m, fn: mean),
        mem: mem
            |> filter(fn: (r) => r.host == "edge-1")
            |> aggregateWindow(every: 1m, fn: mean),
    },
    on: ["_time"],
)
    |> yield(name: "joined_health")

union(
    tables: [
        cpu
            |> keep(columns: ["_time", "host", "region", "_value"])
            |> rename(columns: {_value: "reading"})
            |> set(key: "metric", value: "cpu"),
        mem
            |> keep(columns: ["_time", "host", "region", "_value"])
            |> rename(columns: {_value: "reading"})
            |> set(key: "metric", value: "mem"),
    ],
)
    |> sort(columns: ["_time", "host", "metric"])
    |> yield(name: "union_samples")

union(
    tables: [
        cpu
            |> aggregateWindow(every: 1m, fn: mean)
            |> keep(columns: ["_time", "host", "region", "_value"])
            |> rename(columns: {_value: "reading"})
            |> set(key: "metric", value: "cpu"),
        mem
            |> aggregateWindow(every: 1m, fn: mean)
            |> keep(columns: ["_time", "host", "region", "_value"])
            |> rename(columns: {_value: "reading"})
            |> set(key: "metric", value: "mem"),
    ],
)
    |> pivot(rowKey: ["_time", "host", "region"], columnKey: ["metric"], valueColumn: "reading")
    |> yield(name: "wide_metrics")
