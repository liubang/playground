import "csv"

option task = {name: "task-driven-rollup", every: 1m, offset: 10s}
option task.owner = "ops"

windowUsage = (<-tables, measurement, field) => tables
    |> filter(fn: (r) => r._measurement == measurement and r._field == field)
    |> group(columns: ["host", "region", "service"])
    |> aggregateWindow(
        every: task.every,
        offset: task.offset,
        fn: mean,
        createEmpty: false,
    )

shapeStatus = (r) => {
    level = if r._value >= 80.0 then "critical" else if r._value >= 70.0 then "warm" else "steady"
    return {
        task: task.name,
        owner: task.owner,
        host: r.host,
        region: r.region,
        service: r.service,
        level: level,
        window_start: r._start,
        window_stop: r._stop,
        window_value: r._value,
    }
}

csv.from(file: "cpp/pl/flux/examples/feature_gallery/data/site_ops.annotated.csv")
    |> windowUsage(measurement: "cpu", field: "usage_user")
    |> map(fn: (r) => shapeStatus(r: r))
    |> yield(name: "task_driven_rollup")
