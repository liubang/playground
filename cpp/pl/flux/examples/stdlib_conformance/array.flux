import "array"

numbers = [1, 2, 3]
extended = array.concat(arr: numbers, v: [4])
doubled = array.map(arr: extended, fn: (x) => x * 2)
table_row = array.from(rows: [{host: "edge-1", value: 10}])
    |> findRecord(fn: (r) => r.host == "edge-1", idx: 0)

{
    contains_two: array.contains(arr: numbers, value: 2),
    total: array.reduce(arr: extended, identity: 0, fn: (x, acc) => acc + x),
    doubled: doubled,
    all_positive: array.all(arr: numbers, fn: (x) => x > 0),
    any_large: array.any(arr: numbers, fn: (x) => x > 2),
    table_host: table_row.host,
}
