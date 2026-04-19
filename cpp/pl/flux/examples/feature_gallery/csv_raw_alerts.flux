import "csv"

csv.from(file: "cpp/pl/flux/examples/feature_gallery/alerts.raw.csv", mode: "raw")
    |> filter(fn: (r) => r.service == "api" and (r.severity == "warn" or r.severity == "crit"))
    |> limit(n: 2)
    |> yield(name: "raw_alerts")
