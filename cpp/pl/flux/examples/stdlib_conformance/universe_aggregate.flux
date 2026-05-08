import "array"

data = array.from(
    bucket: "aggregate",
    rows: [
        {_time: 2024-01-01T00:00:00Z, host: "edge-1", _value: 10.0},
        {_time: 2024-01-01T00:01:00Z, host: "edge-1", _value: 15.0},
        {_time: 2024-01-01T00:02:00Z, host: "edge-1", _value: 20.0},
        {_time: 2024-01-01T00:03:00Z, host: "edge-2", _value: 5.0},
        {_time: 2024-01-01T00:04:00Z, host: "edge-2", _value: 5.0},
    ],
)

grouped = data |> group(columns: ["host"])
edge1 = grouped |> filter(fn: (r) => r.host == "edge-1")
edge2 = grouped |> filter(fn: (r) => r.host == "edge-2")

edge1_count = edge1 |> count() |> findRecord(fn: (r) => r.host == "edge-1", idx: 0)
edge1_spread = edge1 |> spread() |> findRecord(fn: (r) => r.host == "edge-1", idx: 0)
edge1_quantile = edge1 |> quantile(q: [0.25, 0.75]) |> findRecord(fn: (r) => r.host == "edge-1", idx: 1)
edge1_median = edge1 |> median() |> findRecord(fn: (r) => r.host == "edge-1", idx: 0)
edge1_first = edge1 |> first() |> findRecord(fn: (r) => r.host == "edge-1", idx: 0)
edge1_last = edge1 |> last() |> findRecord(fn: (r) => r.host == "edge-1", idx: 0)
edge1_top = edge1 |> top(n: 1) |> findRecord(fn: (r) => r.host == "edge-1", idx: 0)
edge1_bottom = edge1 |> bottom(n: 1) |> findRecord(fn: (r) => r.host == "edge-1", idx: 0)
edge1_reduced = edge1
    |> reduce(identity: {total: 0.0}, fn: (r, accumulator) => ({total: accumulator.total + r._value}))
    |> findRecord(fn: (r) => exists r.total, idx: 0)
edge2_distinct = edge2
    |> distinct(column: "_value")
    |> count(column: "_value")
    |> findRecord(fn: (r) => r.host == "edge-2", idx: 0)

{
    sum: sum([1, 2, 3]),
    mean: mean([2.0, 4.0, 6.0]),
    min: min([3, 1, 2]),
    max: max([3, 1, 2]),
    count: edge1_count._value,
    spread: edge1_spread._value,
    quantile: edge1_quantile._value,
    median: edge1_median._value,
    first: edge1_first._value,
    last: edge1_last._value,
    top: edge1_top._value,
    bottom: edge1_bottom._value,
    reduced: edge1_reduced.total,
    distinct_rows: edge2_distinct._value,
}
