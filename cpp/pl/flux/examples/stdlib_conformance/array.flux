import "array"

numbers = [1, 2, 3]
doubled = array.map(arr: numbers, fn: (x) => x * 2)

{
    contains_two: array.contains(arr: numbers, value: 2),
    total: array.reduce(arr: numbers, identity: 0, fn: (x, acc) => acc + x),
    doubled: doubled,
    all_positive: array.all(arr: numbers, fn: (x) => x > 0),
    any_large: array.any(arr: numbers, fn: (x) => x > 2),
}

