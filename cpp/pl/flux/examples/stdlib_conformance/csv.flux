import "csv"

row = csv.from(
    csv: "_time,host,_value\n2024-01-01T00:00:00Z,edge-1,10\n2024-01-01T00:01:00Z,edge-2,12\n",
    mode: "raw",
)
    |> findRecord(fn: (r) => r.host == "edge-2", idx: 0)

{
    host: row.host,
    value: row._value,
}

