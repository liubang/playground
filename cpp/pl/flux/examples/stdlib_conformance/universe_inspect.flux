data = from(
    bucket: "inspect",
    rows: [
        {_time: 2024-01-01T00:00:00Z, host: "edge-1", region: "west", _value: 10.0},
        {_time: 2024-01-01T00:01:00Z, host: "edge-2", region: "east", _value: 20.0},
    ],
)
    |> group(columns: ["region"])

column_names = data |> columns() |> findColumn(fn: (r) => true, column: "_value")
key_names = data |> keys() |> findColumn(fn: (r) => true, column: "_value")
values = data |> findColumn(fn: (r) => r.region == "west", column: "_value")
record = data |> findRecord(fn: (r) => r.host == "edge-2", idx: 0)
named = data |> yield(name: "inspect_result")
plan = data |> explain()

{
    columns: column_names,
    keys: key_names,
    values: values,
    record_host: record.host,
    named_string: string(named),
    plan: plan,
}
