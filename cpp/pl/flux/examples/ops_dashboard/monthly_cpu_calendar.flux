import "csv"

csv.from(file: "cpp/pl/flux/examples/ops_dashboard/cpu_monthly_usage.annotated.csv")
    |> filter(fn: (r) => r.host == "edge-1" and r.region == "us-east")
    |> aggregateWindow(every: 1mo, fn: mean)
    |> yield(name: "monthly_cpu_calendar")
