import "array"

numbers = [1, 2, 3]
extended = array.concat(arr: numbers, v: [4])
doubled = array.map(arr: extended, fn: (x) => x * 2)
range_values = array.range(start: 0, stop: 6, step: 2)
scan_values = array.scan(arr: numbers, identity: {sum: 0}, fn: (x, acc) => ({sum: acc.sum + x}))
unfold_values = array.unfold(
    seed: {value: 1},
    fn: (state) => ({
        value: state.value,
        state: {value: state.value + 1},
        done: state.value > 3,
    }),
)
table_row = array.from(rows: [{host: "edge-1", value: 10}])
    |> findRecord(fn: (r) => r.host == "edge-1", idx: 0)

{
    contains_two: array.contains(arr: numbers, value: 2),
    total: array.reduce(arr: extended, identity: 0, fn: (x, acc) => acc + x),
    doubled: doubled,
    all_positive: array.all(arr: numbers, fn: (x) => x > 0),
    any_large: array.any(arr: numbers, fn: (x) => x > 2),
    range: range_values,
    repeated: array.repeat(value: "x", n: 3),
    length: array.length(arr: extended),
    get: array.get(arr: extended, index: -1),
    get_default: array.get(arr: extended, index: 99, default: -1),
    slice: array.slice(arr: extended, start: 1, end: 3),
    sorted: array.sort(arr: [3, 1, 2], desc: true),
    flat_mapped: array.flatMap(arr: [1, 2], fn: (x) => [x, x * 10]),
    found: array.find(arr: extended, fn: (x) => x > 2),
    missing: array.find(arr: extended, fn: (x) => x > 9, default: -1),
    found_index: array.findIndex(arr: extended, fn: (x) => x > 2 and x < 4),
    taken: array.take(arr: extended, n: 2),
    dropped: array.drop(arr: extended, n: 2),
    reversed: array.reverse(arr: numbers),
    unique: array.unique(arr: [1, 2, 1, 3, 2]),
    unfolded: unfold_values,
    scanned_last: scan_values[2].sum,
    zipped: array.zip(left: ["a", "b"], right: [1, 2]),
    enumerated: array.enumerate(arr: ["a", "b"]),
    table_host: table_row.host,
}
