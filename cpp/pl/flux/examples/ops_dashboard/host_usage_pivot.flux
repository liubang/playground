import "csv"

cpu = csv.from(file: "cpp/pl/flux/examples/ops_dashboard/cpu_usage.annotated.csv")
    |> aggregateWindow(every: 1m, fn: mean)
    |> keep(columns: ["_time", "host", "region", "_value"])
    |> rename(columns: {_value: "usage"})
    |> set(key: "metric", value: "cpu")

mem = csv.from(file: "cpp/pl/flux/examples/ops_dashboard/mem_usage.annotated.csv")
    |> aggregateWindow(every: 1m, fn: mean)
    |> keep(columns: ["_time", "host", "region", "_value"])
    |> rename(columns: {_value: "usage"})
    |> set(key: "metric", value: "mem")

union(tables: [cpu, mem])
    |> sort(columns: ["_time", "host"])
    |> pivot(rowKey: ["_time", "host", "region"], columnKey: ["metric"], valueColumn: "usage")
    |> yield(name: "host_usage_pivot")
