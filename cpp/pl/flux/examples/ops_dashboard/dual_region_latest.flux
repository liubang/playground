import "csv"

west_cpu = csv.from(file: "cpp/pl/flux/examples/ops_dashboard/data/cpu_usage.annotated.csv")
    |> filter(fn: (r) => r.region == "us-west")
    |> sort(columns: ["_time"])
    |> last()
    |> yield(name: "latest_west_cpu")

east_mem = csv.from(file: "cpp/pl/flux/examples/ops_dashboard/data/mem_usage.annotated.csv")
    |> filter(fn: (r) => r.region == "us-east")
    |> sort(columns: ["_time"])
    |> last()
    |> yield(name: "latest_east_mem")
