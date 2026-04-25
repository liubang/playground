import "json"

json.encode(v: {
    host: "edge-1",
    label: "edge \"one\"",
    path: "cpu\\usage",
    ok: true,
    count: 3,
    values: [1.5, false, "cpu"],
    at: 2024-01-01T00:00:00Z,
})
