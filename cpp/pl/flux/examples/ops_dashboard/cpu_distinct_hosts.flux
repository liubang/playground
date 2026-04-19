import "csv"

csv.from(file: "cpp/pl/flux/examples/ops_dashboard/cpu_usage.annotated.csv")
    |> keep(columns: ["host", "region", "_value"])
    |> distinct(column: "host")
    |> yield(name: "cpu_distinct_hosts")
