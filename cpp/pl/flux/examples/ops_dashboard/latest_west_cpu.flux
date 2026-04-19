import "csv"

csv.from(file: "cpp/pl/flux/examples/ops_dashboard/cpu_usage.annotated.csv")
    |> filter(fn: (r) => r.region == "us-west")
    |> sort(columns: ["_time"])
    |> last()
    |> yield(name: "latest_west_cpu")
