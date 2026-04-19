import "csv"

cpu = csv.from(file: "cpp/pl/flux/examples/ops_dashboard/cpu_usage.annotated.csv")
    |> keep(columns: ["_time", "host", "region", "_value"])
    |> rename(columns: {_value: "usage"})
    |> set(key: "kind", value: "cpu")

mem = csv.from(file: "cpp/pl/flux/examples/ops_dashboard/mem_usage.annotated.csv")
    |> keep(columns: ["_time", "host", "region", "_value"])
    |> rename(columns: {_value: "usage"})
    |> set(key: "kind", value: "mem")

union(tables: [cpu, mem])
    |> sort(columns: ["usage"], desc: true)
    |> limit(n: 4)
    |> yield(name: "fleet_usage")
