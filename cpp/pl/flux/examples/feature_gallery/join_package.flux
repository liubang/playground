import "join"

cpu = from(bucket: "cpu", rows: [
    {_time: "t1", host: "edge-1", region: "us-east", _value: 81.0},
    {_time: "t2", host: "edge-1", region: "us-east", _value: 88.0},
    {_time: "t3", host: "edge-2", region: "us-west", _value: 72.0},
])
    |> group(columns: ["host"])

mem = from(bucket: "mem", rows: [
    {_time: "t1", host: "edge-1", region: "us-east", _value: 63.0},
    {_time: "t3", host: "edge-2", region: "us-west", _value: 55.0},
    {_time: "t4", host: "edge-3", region: "eu-central", _value: 40.0},
])
    |> group(columns: ["host"])

join.inner(
    left: cpu,
    right: mem,
    on: (l, r) => l._time == r._time and l.host == r.host,
    as: (l, r) => ({
        _time: l._time,
        host: l.host,
        region: l.region,
        cpu: l._value,
        mem: r._value,
        pressure: l._value + r._value,
    }),
)
    |> yield(name: "join_package_inner")

join.left(
    left: cpu,
    right: mem,
    on: (l, r) => l._time == r._time and l.host == r.host,
    as: (l, r) => ({
        _time: l._time,
        host: l.host,
        region: l.region,
        cpu: l._value,
        mem: r._value,
    }),
)
    |> yield(name: "join_package_left")

join.right(
    left: cpu,
    right: mem,
    on: (l, r) => l._time == r._time and l.host == r.host,
    as: (l, r) => ({
        _time: r._time,
        host: r.host,
        region: r.region,
        cpu: l._value,
        mem: r._value,
    }),
)
    |> yield(name: "join_package_right")

join.full(
    left: cpu,
    right: mem,
    on: (l, r) => l._time == r._time and l.host == r.host,
    as: (l, r) => ({
        left_time: l._time,
        right_time: r._time,
        left_host: l.host,
        right_host: r.host,
        left_region: l.region,
        right_region: r.region,
        cpu: l._value,
        mem: r._value,
    }),
)
    |> yield(name: "join_package_full")
