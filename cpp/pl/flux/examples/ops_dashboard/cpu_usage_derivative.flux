import "csv"

cpu_usage_derivative = csv.from(
    file: "cpp/pl/flux/examples/ops_dashboard/cpu_usage.annotated.csv",
)
    |> group(columns: ["host", "region"])
    |> sort(columns: ["host", "_time"])
    |> derivative(unit: 1s)
    |> yield(name: "cpu_usage_derivative")
