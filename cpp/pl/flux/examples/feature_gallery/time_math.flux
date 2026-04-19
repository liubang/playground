import "csv"

csv.from(file: "cpp/pl/flux/examples/feature_gallery/site_ops.annotated.csv")
    |> filter(fn: (r) => r._measurement == "cpu")
    |> range(start: 2024-06-01T09:00:00Z, stop: 2024-06-01T09:02:00Z)
    |> group(columns: ["host"])
    |> sort(columns: ["_time"])
    |> elapsed(unit: 10s, columnName: "ticks")
    |> yield(name: "cpu_elapsed")

csv.from(file: "cpp/pl/flux/examples/feature_gallery/service_counters.annotated.csv")
    |> group(columns: ["host"])
    |> sort(columns: ["_time"])
    |> difference(nonNegative: true, keepFirst: true)
    |> yield(name: "request_difference")

csv.from(file: "cpp/pl/flux/examples/feature_gallery/service_counters.annotated.csv")
    |> group(columns: ["host"])
    |> sort(columns: ["_time"])
    |> derivative(unit: 30s, nonNegative: true, initialZero: true)
    |> yield(name: "request_derivative")
