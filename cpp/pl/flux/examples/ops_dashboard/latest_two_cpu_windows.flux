import "csv"

csv.from(file: "cpp/pl/flux/examples/ops_dashboard/cpu_usage.annotated.csv")
    |> keep(columns: ["_time", "host", "region", "_value"])
    |> tail(n: 2, offset: 1)
    |> yield(name: "latest_two_cpu_windows")
