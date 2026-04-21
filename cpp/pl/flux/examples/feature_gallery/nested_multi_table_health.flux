import "array"
import "csv"

option task = {name: "nested-multi-table-health", every: 1m, offset: 10s}
option task.owner = "ops"

data = csv.from(file: "cpp/pl/flux/examples/feature_gallery/data/site_ops.annotated.csv")

baseWatchlist = [
    {host: "edge-1", owner: task.owner, cohort: "primary"},
    {host: "edge-2", owner: task.owner, cohort: "primary"},
]

extraWatchlist = data
    |> filter(fn: (r) => r._measurement == "cpu" and r.service == "api")
    |> findColumn(fn: (r) => r.host == "edge-3", column: "host")
    |> array.map(fn: (host) => ({host: host, owner: task.owner, cohort: "canary"}))

watchlist = baseWatchlist
    |> array.concat(v: extraWatchlist)
    |> array.filter(fn: (r) => r.host != "")

watchHosts = watchlist |> array.map(fn: (r) => r.host)
watchSummary = watchHosts
    |> array.reduce(
        identity: {count: 0, last: ""},
        fn: (host, accumulator) => ({
            count: accumulator.count + 1,
            last: host,
        }),
    )

hasCanary = watchHosts |> array.contains(value: "edge-3")
allWatchHostsNamed = watchHosts |> array.all(fn: (host) => host != "")

windowMetric = (<-tables, measurement, field) => tables
    |> filter(
        fn: (r) => r._measurement == measurement and
            r._field == field and
            array.contains(arr: watchHosts, value: r.host),
    )
    |> group(columns: ["host", "region", "service"])
    |> aggregateWindow(
        every: task.every,
        offset: task.offset,
        fn: mean,
        createEmpty: false,
    )

shapeHealth = (r) => {
    pressure = if r._value_cpu >= 80.0 or r._value_mem >= 65.0 then "hot"
        else if r._value_cpu >= 70.0 or r._value_mem >= 55.0 then "warm"
        else "steady"
    return {
        task: task.name,
        owner: task.owner,
        host: r.host,
        region: r.region,
        service: r.service,
        _time: r._time,
        pressure: pressure,
        cpu_mean: r._value_cpu,
        mem_mean: r._value_mem,
        watch_count: watchSummary.count,
        watch_last: watchSummary.last,
        has_canary: hasCanary,
        all_named_hosts: allWatchHostsNamed,
    }
}

cpuWindows = data
    |> windowMetric(measurement: "cpu", field: "usage_user")
    |> group(columns: ["host", "region", "service"])

memWindows = data
    |> windowMetric(measurement: "mem", field: "used_percent")
    |> group(columns: ["host", "region", "service"])

health = join(
    tables: {cpu: cpuWindows, mem: memWindows},
    method: "inner",
    on: ["_time", "host", "region", "service"],
)
    |> map(fn: (r) => shapeHealth(r: r))

inventory = array.from(rows: watchlist)
    |> group(columns: ["host"])

enrichedHealth = join(
    tables: {
        inventory: inventory,
        health: health |> group(columns: ["host"]),
    },
    method: "inner",
    on: ["host"],
)
    |> map(
        fn: (r) => ({
            r with cohort_label: if r.cohort == "canary" then "watch-canary" else "watch-primary"
        }),
    )

cpuReadings = enrichedHealth
    |> map(
        fn: (r) => ({
            host: r.host,
            region: r.region,
            service: r.service,
            cohort: r.cohort_label,
            pressure: r.pressure,
            metric: "cpu",
            reading: r.cpu_mean,
        }),
    )

memReadings = enrichedHealth
    |> map(
        fn: (r) => ({
            host: r.host,
            region: r.region,
            service: r.service,
            cohort: r.cohort_label,
            pressure: r.pressure,
            metric: "mem",
            reading: r.mem_mean,
        }),
    )

union(
    tables: [cpuReadings, memReadings],
)
    |> sort(columns: ["host", "metric"])
    |> yield(name: "unioned_health")

union(
    tables: [cpuReadings, memReadings],
)
    |> pivot(
        rowKey: ["host", "region", "service", "cohort", "pressure"],
        columnKey: ["metric"],
        valueColumn: "reading",
    )
    |> yield(name: "wide_health")

enrichedHealth
    |> filter(fn: (r) => r.cohort == "canary" or r.pressure == "hot")
    |> yield(name: "watchlist_focus")
