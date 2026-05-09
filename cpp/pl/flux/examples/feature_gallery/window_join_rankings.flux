import "array"

cpu = array.from(bucket: "cpu", rows: [
    {_time: 2024-01-01T00:00:10Z, host: "a", region: "east", _value: 10.0},
    {_time: 2024-01-01T00:00:40Z, host: "a", region: "east", _value: 25.0},
    {_time: 2024-01-01T00:02:10Z, host: "a", region: "east", _value: 35.0},
    {_time: 2024-01-01T00:00:20Z, host: "b", region: "west", _value: 50.0},
    {_time: 2024-01-01T00:01:20Z, host: "b", region: "west", _value: 70.0},
])

windowed_cpu = cpu
    |> range(start: 2024-01-01T00:00:00Z, stop: 2024-01-01T00:03:00Z)
    |> group(columns: ["host"])
    |> window(every: 1m, createEmpty: true)
    |> yield(name: "windowed_cpu")

cpu_points = array.from(bucket: "cpu", rows: [
    {_time: "t1", host: "a", region: "east", _value: 90.0},
    {_time: "t2", host: "a", region: "east", _value: 91.0},
    {_time: "t4", host: "b", region: "west", _value: 92.0},
])

mem_points = array.from(bucket: "mem", rows: [
    {_time: "t2", host: "a", region: "east", _value: 40.0},
    {_time: "t3", host: "a", region: "east", _value: 20.0},
    {_time: "t4", host: "b", region: "west", _value: 55.0},
])

full_joined = join(
    tables: {cpu: cpu_points, mem: mem_points},
    method: "full",
    on: ["_time", "host"],
)
    |> yield(name: "full_joined")

left_joined = join(
    tables: {cpu: cpu_points, mem: mem_points},
    method: "left",
    on: ["_time", "host"],
)
    |> yield(name: "left_joined")

per_host_spread = cpu
    |> group(columns: ["host"])
    |> spread()
    |> yield(name: "per_host_spread")

percentile_75 = cpu
    |> quantile(q: 0.75)
    |> yield(name: "percentile_75")

common_percentiles = cpu
    |> quantile(q: [0.5, 0.75, 0.99, 0.999])
    |> yield(name: "common_percentiles")

median_usage = cpu
    |> median()
    |> yield(name: "median_usage")

top_usage = cpu
    |> group(columns: ["host"])
    |> top(n: 1)
    |> yield(name: "top_usage")

bottom_usage = cpu
    |> group(columns: ["host"])
    |> bottom(n: 1)
    |> yield(name: "bottom_usage")
