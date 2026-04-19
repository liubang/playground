import "csv"

csv.from(file: "cpp/pl/flux/examples/ops_dashboard/cpu_usage.annotated.csv")
    |> filter(fn: (r) => r.host == "edge-1" and r.region == "us-east")
    |> reduce(
        identity: {samples: 0, total: 0.0},
        fn: (r, accumulator) => ({
            samples: accumulator.samples + 1,
            total: accumulator.total + r._value,
        }),
    )
    |> yield(name: "edge1_cpu_rollup")
