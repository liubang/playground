import "csv"

ops = csv.from(file: "cpp/pl/flux/examples/feature_gallery/multi_block.annotated.csv")

ops
    |> columns()
    |> yield(name: "ops_columns")

ops
    |> group(columns: ["region", "host"])
    |> keys()
    |> yield(name: "ops_keys")

{
    eastHosts: findColumn(tables: ops, fn: (r) => r.region == "us-east", column: "host"),
    westFirst: findRecord(tables: ops, fn: (r) => r.region == "us-west", idx: 0),
}
