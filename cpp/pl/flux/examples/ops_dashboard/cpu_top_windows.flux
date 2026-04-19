import "csv"

csv.from(file: "cpp/pl/flux/examples/ops_dashboard/cpu_usage.annotated.csv")
    |> aggregateWindow(every: 1m, fn: mean)
    |> sort(columns: ["_value"], desc: true)
    |> limit(n: 3)
    |> yield(name: "cpu_top_windows")
