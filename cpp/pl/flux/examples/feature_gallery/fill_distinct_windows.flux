import "csv"

cpu = csv.from(file: "cpp/pl/flux/examples/feature_gallery/site_ops.annotated.csv")
    |> filter(fn: (r) => r._measurement == "cpu" and r.host == "edge-1")

cpu
    |> keep(columns: ["host"])
    |> distinct(column: "host")
    |> yield(name: "distinct_hosts")

cpu
    |> range(start: 2024-06-01T09:00:00Z, stop: 2024-06-01T09:01:20Z)
    |> aggregateWindow(every: 20s, fn: mean, createEmpty: true)
    |> fill(usePrevious: true)
    |> yield(name: "filled_windows")

cpu
    |> aggregateWindow(every: 20s, fn: mean, offset: 10s, createEmpty: false)
    |> yield(name: "offset_windows")
