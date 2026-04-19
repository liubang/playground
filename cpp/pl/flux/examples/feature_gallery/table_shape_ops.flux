import "csv"

csv.from(file: "cpp/pl/flux/examples/feature_gallery/site_ops.annotated.csv")
    |> filter(fn: (r) => r._measurement == "cpu")
    |> duplicate(column: "_value", as: "original")
    |> rename(columns: {_measurement: "measurement", _field: "field"})
    |> set(key: "cluster", value: "primary")
    |> map(fn: (r) => ({
        r with
        usage_band: (if r._value >= 80.0 then "peak" else "normal"),
    }))
    |> drop(columns: ["status"])
    |> keep(
        columns: [
            "_time",
            "host",
            "region",
            "service",
            "measurement",
            "field",
            "_value",
            "original",
            "cluster",
            "usage_band",
        ],
    )
    |> sort(columns: ["host", "_time"])
    |> limit(n: 5)
    |> tail(n: 3)
    |> yield(name: "table_shape_ops")
