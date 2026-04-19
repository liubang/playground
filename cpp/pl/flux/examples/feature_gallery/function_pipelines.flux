option deployment = {name: "feature-gallery"}

hotThreshold = 75.0

decorate = (r, ?prefix = "svc") => ({
    r with
    label: "${prefix}-${r.host}",
    state: (if r._value >= hotThreshold then "hot" else "steady"),
    matched: (r.host =~ /edge-[13]/),
})

stamp = (<-tables, ?env = "prod") => tables |> set(key: "env", value: env)

from(
    bucket: "inline",
    rows: [
        {_time: "2024-06-01T09:00:00Z", host: "edge-1", _value: 71.0},
        {_time: "2024-06-01T09:00:30Z", host: "edge-2", _value: 64.0},
        {_time: "2024-06-01T09:01:10Z", host: "edge-3", _value: 88.0},
    ],
)
    |> map(fn: (r) => decorate(r: r, prefix: "node"))
    |> filter(fn: (r) => r.matched or r.state == "hot")
    |> stamp(env: "staging")
    |> yield(name: "function_pipelines")
