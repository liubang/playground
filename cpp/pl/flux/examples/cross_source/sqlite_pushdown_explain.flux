import "sqlite"

data = sqlite.from(
    path: "cpp/pl/flux/examples/cross_source/metrics.db",
    table: "cpu",
)
    |> range(start: 2024-07-01T10:00:00Z, stop: 2024-07-01T10:04:00Z)
    |> filter(fn: (r) => r.host == "edge-1")
    |> filter(fn: (r) => r.usage > 80.0)
    |> drop(columns: ["region"])
    |> rename(columns: {usage: "value"})
    |> keep(columns: ["_time", "host", "value"])
    |> sort(columns: ["value"], desc: true)
    |> limit(n: 10)

services = sqlite.from(
    path: "cpp/pl/flux/examples/cross_source/metrics.db",
    table: "cpu",
)
    |> rename(columns: {host: "service"})
    |> distinct(column: "service")
    |> keep(columns: ["service"])
    |> sort(columns: ["service"], desc: false)

optimizedGraph = data |> explain(optimized: true, graph: true)
physicalGraph = data |> explain(physical: true, graph: true)
pipelineGraph = services |> explain(pipeline: true, graph: true)

{
    values: data |> findColumn(fn: (r) => true, column: "value"),
    services: services |> findColumn(fn: (r) => true, column: "service"),
    plan: data |> explain(),
    optimizedGraph: optimizedGraph,
    physicalPlan: data |> explain(physical: true),
    physicalGraph: physicalGraph,
    pipelineGraph: pipelineGraph,
    distinctPlan: services |> explain(),
    distinctPhysicalPlan: services |> explain(physical: true),
}
