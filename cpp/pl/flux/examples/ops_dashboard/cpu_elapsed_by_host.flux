import "csv"

cpu_elapsed_by_host = csv.from(
    file: "cpp/pl/flux/examples/ops_dashboard/cpu_usage.annotated.csv",
)
    |> group(columns: ["host", "region"])
    |> sort(columns: ["host", "_time"])
    |> elapsed(unit: 1s)
    |> yield(name: "cpu_elapsed_by_host")
