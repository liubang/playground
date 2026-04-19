import "csv"

join(
    tables: {
        cpu: csv.from(file: "cpp/pl/flux/examples/ops_dashboard/cpu_usage.annotated.csv")
            |> filter(fn: (r) => r.host == "edge-1" and r.region == "us-east")
            |> aggregateWindow(every: 1m, fn: mean),
        mem: csv.from(file: "cpp/pl/flux/examples/ops_dashboard/mem_usage.annotated.csv")
            |> filter(fn: (r) => r.host == "edge-1" and r.region == "us-east")
            |> aggregateWindow(every: 1m, fn: mean),
    },
    on: ["_time"],
)
    |> yield(name: "host_health")
