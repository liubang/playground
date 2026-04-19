import "csv"

cpu = csv.from(file: "cpp/pl/flux/examples/feature_gallery/site_ops.annotated.csv")
    |> filter(fn: (r) => r._measurement == "cpu")
    |> group(columns: ["service"])
    |> sort(columns: ["_time"])

cpu
    |> first()
    |> yield(name: "cpu_first")

cpu
    |> last()
    |> yield(name: "cpu_last")

cpu
    |> count(column: "_value")
    |> yield(name: "cpu_count")

csv.from(file: "cpp/pl/flux/examples/feature_gallery/site_ops.annotated.csv")
    |> filter(fn: (r) => r._measurement == "cpu" and r.host == "edge-1")
    |> reduce(
        identity: {samples: 0, total: 0.0, peak: 0.0},
        fn: (r, accumulator) => ({
            samples: accumulator.samples + 1,
            total: accumulator.total + r._value,
            peak: (if r._value > accumulator.peak then r._value else accumulator.peak),
        }),
    )
    |> yield(name: "edge1_cpu_reduce")
