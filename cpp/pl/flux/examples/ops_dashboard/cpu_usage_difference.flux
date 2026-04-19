import "csv"

cpu_usage_difference = csv.from(
    file: "cpp/pl/flux/examples/ops_dashboard/cpu_usage.annotated.csv",
)
    |> group(columns: ["host", "region"])
    |> sort(columns: ["host", "_time"])
    |> difference()
    |> yield(name: "cpu_usage_difference")
