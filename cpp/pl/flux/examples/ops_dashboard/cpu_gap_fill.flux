import "csv"

csv.from(file: "cpp/pl/flux/examples/ops_dashboard/cpu_usage.annotated.csv")
    |> filter(fn: (r) => r.host == "edge-1")
    |> aggregateWindow(every: 30s, fn: mean, createEmpty: true)
    |> fill(usePrevious: true)
    |> yield(name: "cpu_gap_fill")
