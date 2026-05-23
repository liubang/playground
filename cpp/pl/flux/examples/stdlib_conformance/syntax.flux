package syntax_gallery

import arr "array"
import "regexp"

option task = {name: "syntax-gallery", every: 1m}
option feature.enabled = true

builtin len : (v: A) => int where A: Equatable

@doc({topic: "syntax"}, ["attribute", "params"], len("ok"))
base = {
    host: "edge-1",
    region: "west",
    usage: 80.5,
    active: true,
}

updated = {base with usage: base.usage + 1.5, service: "api"}
numbers = [1, 2, 3, 4]
lookup = ["cpu": 10, "mem": 20]
empty = {object: {}, array: [], dict: [:]}
label = "${updated.host}/${updated.service}"
regex = regexp.compile(v: "^edge")
truthy = true
falsy = false

decorate = (r, ?prefix = "svc") => ({
    r with
    label: "${prefix}-${r.host}",
    hot: r.usage >= 80.0,
})

score = (value) => {
    adjusted = value + 3
    return adjusted * 2
}

pipe_add = (<-value, ?inc = 1) => value + inc
apply = (value, fn) => fn(value)
apply_twice = (value, fn) => fn(fn(value))
make_adder = (delta) => (value) => value + delta
add_five = make_adder(delta: 5)
callbacks = {
    classify: (r) => (if r.usage >= 80.0 then "hot" else "steady"),
    bump: (x) => x + 1,
}
transforms = [
    (x) => x + 1,
    (x) => x * 2,
]
fib = (steps) => {
    state = steps |> arr.reduce(
        identity: {prev: 0, curr: 1},
        fn: (x, acc) => ({prev: acc.curr, curr: acc.prev + acc.curr}),
    )
    return state.prev
}
factorial = (values) =>
    values |> arr.reduce(identity: {value: 1}, fn: (x, acc) => ({value: acc.value * x}))
max_value = (values) =>
    values |> arr.reduce(
        identity: {value: -999},
        fn: (x, acc) => ({value: (if x > acc.value then x else acc.value)}),
    )

testcase syntax_smoke extends "stdlib" {
    local = score(value: 4)
    return local > 13
}

rows = arr.from(
    bucket: "syntax",
    rows: [
        {_time: "2024-01-01T00:00:00Z", host: "edge-1", region: "west", usage: 80.5},
        {_time: "2024-01-01T00:01:00Z", host: "edge-2", region: "east", usage: 41.0},
    ],
)

hot = rows
    |> map(fn: (r) => decorate(r: r, prefix: "node"))
    |> filter(fn: (r) => r.hot and r.host =~ regex)
    |> findRecord(fn: (r) => exists r.label, idx: 0)

{
    option_name: task.name,
    option_member: feature.enabled,
    member: updated.host,
    index_array: numbers[2],
    index_object: updated["service"],
    dict_cpu: lookup["cpu"],
    empty_object_len: len(empty.object),
    string_interp: label,
    numeric: {
        add: 1 + 2,
        sub: 5 - 3,
        mul: 2 * 4,
        div: 7 / 2,
        mod: 7 % 3,
        unary: -3,
        duration: string(-5m),
        uint: string(4u),
    },
    comparisons: {
        lt: 1 < 2,
        lte: 2 <= 2,
        gt: 3 > 2,
        gte: 3 >= 3,
        eq: "a" == "a",
        neq: "a" != "b",
        regex: "edge-1" =~ /^edge/,
        not_regex: "edge-1" !~ /west/,
    },
    logical: not falsy and (truthy or falsy),
    conditional: (if exists updated.service then "present" else "missing"),
    closure_default: decorate(r: base).label,
    functions: {
        positional_call: score(4),
        named_call: score(value: 4),
        closure_return: add_five(value: 3),
        higher_order_arg: apply(value: 4, fn: (x) => x * 3),
        higher_order_nested: apply_twice(value: 2, fn: (x) => x + 3),
        object_function: callbacks.classify(r: updated),
        indexed_function: transforms[1](3),
        inline_function: ((x) => x + 1)(2),
        pipe_function: 4 |> pipe_add(inc: 3),
        fibonacci_6: fib(steps: [1, 2, 3, 4, 5, 6]),
        factorial_5: factorial(values: [1, 2, 3, 4, 5]).value,
        max_value: max_value(values: [3, 9, 4, 7]).value,
    },
    array_hof: (numbers |> arr.map(fn: (x) => x * 2) |> arr.filter(fn: (x) => x > 4)),
    table_hof: {host: hot.host, label: hot.label, hot: hot.hot},
}
