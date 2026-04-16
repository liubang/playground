# Flux Parser and Runtime Foundations

`cpp/pl/flux` now provides:

- an AST debugging entrypoint that can print Flux programs as tree text or JSON
- a parser test suite that covers a growing realistic Flux subset
- runtime foundations for interpretation: value model, environment/scope, expression evaluation, and first-pass statement execution

AST dump formats:

- Tree text: best for manual inspection
- JSON: best for scripts, regression checks, and snapshots

For a feature-by-feature implementation overview, see [SUPPORT_MATRIX.md](./SUPPORT_MATRIX.md).

## Build

```bash
bazel build //cpp/pl/flux:flux
```

Optional:

```bash
bazel build //cpp/pl/flux:parser_test
```

## Usage

Read from stdin:

```bash
./bazel-bin/cpp/pl/flux/flux ast <<'EOF'
package metrics
import "array"
config = {base with enabled: true, tags: ["cpu", "mem"]}
status = if exists config.enabled then "ok" else "missing"
EOF
```

Read from a file:

```bash
./bazel-bin/cpp/pl/flux/flux ast path/to/query.flux
```

Dump AST from inline source:

```bash
./bazel-bin/cpp/pl/flux/flux ast -e 'value = 1 + 2'
```

Execute Flux source with the runtime CLI:

```bash
./bazel-bin/cpp/pl/flux/flux -e 'sum([1, 2, 3])'
```

Run a Flux source file:

```bash
cat > /tmp/query.flux <<'EOF'
import "csv"

data = csv.from(
    csv: "_time,_measurement,_value\n2024-01-01T00:00:00Z,cpu,95.5\n",
    mode: "raw",
)
    |> filter(fn: (r) => r._measurement == "cpu")
    |> limit(n: 1)
EOF

./bazel-bin/cpp/pl/flux/flux /tmp/query.flux
```

Start the REPL:

```bash
./bazel-bin/cpp/pl/flux/flux
```

The REPL keeps one shared environment, so later lines can use earlier bindings:

```text
flux> x = 40
40
flux> x + 2
42
flux> :quit
```

By default, runtime execution installs the current builtin prelude and prints the last evaluated value. Use `--quiet` to suppress value output, or `--no-prelude` to execute only explicitly declared/imported symbols.

Output JSON:

```bash
./bazel-bin/cpp/pl/flux/flux ast --json <<'EOF'
package metrics
import "array"
config = {base with enabled: true, tags: ["cpu", "mem"]}
status = if exists config.enabled then "ok" else "missing"
EOF
```

## Example

Input:

```flux
package metrics
import "array"
config = {base with enabled: true, tags: ["cpu", "mem"]}
status = if exists config.enabled then "ok" else "missing"
```

Tree output:

```text
File name="<stdin>"
PackageClause name=metrics
ImportDeclaration path="array"
VariableAssignment id=config
|  `- ObjectExpr with=base
|     |- Property key=enabled
|     |  `- BooleanLit value=true
|     `- Property key=tags
|        `- ArrayExpr
|           |- StringLit value="cpu"
|           `- StringLit value="mem"
VariableAssignment id=status
   `- ConditionalExpr
      |- UnaryExpr op=exists
      |  `- MemberExpr property=enabled
      |     `- Identifier name=config
      |- StringLit value="ok"
      `- StringLit value="missing"
```

JSON output:

```json
{"type":"File","summary":"name=<stdin>","children":[{"type":"PackageClause","summary":"name=metrics","children":[]},{"type":"ImportDeclaration","summary":"path=\"array\"","children":[]},{"type":"VariableAssignment","summary":"id=config","children":[{"type":"ObjectExpr","summary":"with=base","children":[{"type":"Property","summary":"key=enabled","children":[{"type":"BooleanLit","summary":"value=true","children":[]}]},{"type":"Property","summary":"key=tags","children":[{"type":"ArrayExpr","summary":"","children":[{"type":"StringLit","summary":"value=\"cpu\"","children":[]},{"type":"StringLit","summary":"value=\"mem\"","children":[]}]}]}]}]},{"type":"VariableAssignment","summary":"id=status","children":[{"type":"ConditionalExpr","summary":"","children":[{"type":"UnaryExpr","summary":"op=exists","children":[{"type":"MemberExpr","summary":"property=enabled","children":[{"type":"Identifier","summary":"name=config","children":[]}]}]},{"type":"StringLit","summary":"value=\"ok\"","children":[]},{"type":"StringLit","summary":"value=\"missing\"","children":[]}]}]}]}
```

## Tests

Run the regression tests directly:

```bash
bazel test //cpp/pl/flux:parser_unit_test
bazel test //cpp/pl/flux:scanner_unit_test
```

Run the parser test with full output:

```bash
bazel test //cpp/pl/flux:parser_unit_test --test_output=all
```

The current unit tests cover these key flows:

- `package` / `import`
- `builtin` + function type + `where`
- `testcase ... extends ...`
- `option task = {...}` programs
- string interpolation
- regex match
- record update `{base with ...}`
- array / dict / boolean / float / duration
- `if exists ... then ... else ...`
- shorthand and block-bodied arrow functions
- realistic `filter` / `map` / `aggregateWindow` / `join` / `union` / `reduce` query shapes
- runtime value creation, deep equality, and object lookup
- runtime environment scope chaining and option lookup
- first-pass expression evaluation for literals, identifiers, arrays/objects, member/index access, unary/binary/logical operators, conditionals, string interpolation, record update, function values, and function calls
- first-pass statement execution for variable assignment, `option` assignment, expression statements, and block/return behavior
- file-level execution across shared top-level environment state
- runtime handling for top-level `builtin` declarations
- package/import metadata handling during file execution
- command-line execution through `flux`, including `-e`, file mode, and a small REPL
- in-memory query-style execution for `from |> range |> filter |> map`, plus `limit`, `keep`, `drop`, `rename`, `duplicate`, `set`, `reduce`, `sort`, `group`, `count`, `first`, `last`, `union`, a lightweight two-table `join`, first-pass `aggregateWindow`, and Flux-style CSV input through `csv.from`
- tree dump / JSON dump

## Runtime Status

The runtime layer is still early, but it now has several concrete building blocks:

- `runtime_value`: runtime scalar/container values such as null, bool, int, uint, float, string, duration, time, regex, array, object, and a lightweight in-memory table value
- `runtime_env`: lexical environments with parent scopes, variable bindings, option bindings, and nearest-scope assignment
- `runtime_builtin`: a small builtin registry with `len`, `string`, `contains`, `sum`, `mean`, `min`, `max`, first-pass query builtins `from`, `range`, `filter`, `map`, `limit`, `keep`, `drop`, `rename`, `duplicate`, `set`, `reduce`, `sort`, `group`, `count`, `first`, `last`, `union`, `join`, and `aggregateWindow`, plus an imported `csv.from` package builtin
- `runtime_eval`: a first expression evaluator for AST expressions, including function values and function calls
- `runtime_exec`: a first statement executor for assignments, `option`, expression statements, and block/return control flow
- `flux_cli`: a small CLI/REPL wrapper around the parser and runtime executor

The current `aggregateWindow` implementation is intentionally lightweight: it buckets RFC3339 `_time` values by fixed durations such as `1m`, preserves the current `_group` marker, supports `column`, and can call `mean`, `sum`, `min`, `max`, custom array functions, or the special window form of `count`. Calendar-aware windows, offsets, time zones, and `createEmpty: true` are not implemented yet.

Current evaluator support includes:

- literals
- identifiers
- arrays and objects
- basic record update with `{base with ...}`
- member and index access
- unary `not`, unary `-`, and `exists`
- arithmetic/comparison/logical expressions
- conditionals
- string interpolation
- regex match
- user-defined function values with lexical closure capture
- builtin and user-defined function calls
- value forwarding through `|>` into builtin functions and user-defined pipe parameters
- small numeric aggregate builtins over arrays
- lightweight in-memory query pipelines using `from`, `range`, `filter`, `map`, `limit`, `keep`, `drop`, `rename`, `duplicate`, `set`, `reduce`, `sort`, `group`, `count`, `first`, `last`, `union`, `join`, and `aggregateWindow`

CSV input is available through the Flux-style `csv` package import. The builtin supports either an inline CSV string through `csv:` or a local file path through `file:`. Its default mode matches Flux's annotated CSV shape (`mode: "annotations"`), while `mode: "raw"` treats the first row as headers and returns every cell as a string:

```flux
import "csv"
builtin filter : (<-tables: stream[A], fn: (r: A) => bool) => stream[A]

data = csv.from(
    csv: "_time,_measurement,_value\n2024-01-01T00:00:00Z,cpu,95.5\n",
    mode: "raw",
)
    |> filter(fn: (r) => r._measurement == "cpu")
```

The same builtin can read a file:

```flux
import "csv"

data = csv.from(file: "path/to/data.csv", mode: "raw")
```

Annotated CSV supports the common Flux metadata rows `#datatype`, `#group`, and optional `#default`. Typed cells are materialized into runtime values for `string`, `long`, `unsignedLong`, `double`, `boolean`, `dateTime:RFC3339`, `dateTime:RFC3339Nano`, and `duration`; group columns are exposed through a lightweight `_group` object on each row:

```flux
import "csv"

data = csv.from(csv: "#datatype,string,long,dateTime:RFC3339,string,double,boolean\n#group,false,false,true,true,false,false\n#default,_result,,,,,\n,result,table,_time,_measurement,_value,active\n,,0,2024-01-01T00:00:00Z,cpu,95.5,true\n")
```

This is still a lightweight in-memory table representation rather than a full Flux stream/table engine, so multi-table annotated CSV boundaries and the full standard-library CSV surface are not complete yet.

Current statement execution support includes:

- variable assignment
- `option` variable assignment
- `option` member assignment such as `option task.offset = 30s`
- expression statements
- block execution with `return` short-circuiting
- file-level execution of top-level statements in shared order
- top-level `builtin` declaration execution for known builtins and placeholder declarations for unknown builtins
- package metadata and import bindings materialized during file execution
- end-to-end file execution for simple in-memory query scripts when the relevant builtins are declared
- CLI execution through `flux -e`, `flux path/to/query.flux`, and interactive REPL input

Not implemented yet in the runtime:

- richer query-oriented pipe semantics beyond the current in-memory table pipeline subset
- full calendar-aware `aggregateWindow` semantics such as offsets, time zones, and empty window materialization
- `testcase` execution
- a broader standard-library builtin catalog

## Notes

- `flux ast` prints parser errors first, then dumps the current AST, and returns a non-zero exit code.
- AST debug output now includes source locations for top-level and several nested nodes, including statements, expressions, blocks, properties, array items, dict items, type constraints, and function/type parameter nodes.
- Source locations are still not universal across every AST node, so some debug summaries remain location-free.
