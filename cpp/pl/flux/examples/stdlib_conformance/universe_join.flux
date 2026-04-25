left = from(
    bucket: "left",
    rows: [
        {host: "edge-1", cpu: 80.0},
        {host: "edge-2", cpu: 60.0},
    ],
)

right = from(
    bucket: "right",
    rows: [
        {host: "edge-1", mem: 70.0},
        {host: "edge-3", mem: 50.0},
    ],
)

row = join(tables: {left: left, right: right}, on: ["host"], method: "inner")
    |> findRecord(fn: (r) => r.host == "edge-1", idx: 0)

{
    host: row.host,
    cpu: row.cpu,
    mem: row.mem,
}
