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

Benchmark helpers for repeatable local capacity checks now live in
[benchmark/README.md](./benchmark/README.md).

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

Choose a structured output format instead of the default human-readable formatter:

```bash
./bazel-bin/cpp/pl/flux/flux --output-format csv -e 'value = 41
value + 1'
```

For query-style scripts, the CLI now renders named result blocks and simple terminal tables:

```text
Result: data
Table: bucket=csv, rows=1
+==================================================+
| _time                  | _measurement | _value |
+==================================================+
| "2024-01-01T00:00:00Z" | "cpu"        | "95.5" |
+==================================================+
```

The same result stream can be exported as lightweight annotated CSV:

```text
#datatype,string,long,string,string,string
#group,false,false,false,false,false
#default,data,,,,
,result,table,_time,_measurement,_value
,data,0,2024-01-01T00:00:00Z,cpu,95.5
```

Or as JSON:

```bash
./bazel-bin/cpp/pl/flux/flux --output-format json -e 'value = 41
value + 1'
```

```json
{"package":null,"imports":[],"results":[{"name":"value","value":41},{"name":"_result","value":42}],"last":42}
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

Run a larger end-to-end example with checked-in CSV data and a reusable query:

```bash
bazel build //cpp/pl/flux:flux
./bazel-bin/cpp/pl/flux/flux cpp/pl/flux/examples/ops_dashboard/query.flux
```

That scenario lives in [examples/ops_dashboard/README.md](./examples/ops_dashboard/README.md) and now includes a small suite of reusable queries covering combinations like `aggregateWindow + join`, `aggregateWindow(createEmpty) + fill`, `group + sort + elapsed`, `group + sort + difference`, `group + sort + derivative`, `aggregateWindow + union + pivot`, calendar `aggregateWindow(1mo)`, `distinct`, `sort + limit`, `tail(offset)`, `union`, `reduce`, `last`, and multi-result scripts that can be narrowed with `--result`.

The current builtin surface also includes lightweight table-inspection helpers such as
`columns()`, `keys()`, `findColumn(fn:, column:)`, and `findRecord(fn:, idx:)`, which are
useful for narrowing and inspecting in-memory query results.

`aggregateWindow()` now covers the common fixed-duration and calendar-window parameter
combinations used by realistic queries: `location`, `timeSrc`, `timeDst`, `period`,
calendar-window `offset`, negative `period`, overlapping windows when `every != period`,
and `createEmpty` behavior across `range()` bounds with selector functions dropping empty
windows.

Start the REPL:

```bash
./bazel-bin/cpp/pl/flux/flux
```

Inside the REPL, `help`, `:help`, or `.help` shows the built-in commands.

The REPL keeps one shared environment, so later lines can use earlier bindings:

```text
flux> x = 40
40
flux> x + 2
42
flux> :quit
```

Multi-line input is buffered until the current expression or statement looks complete:

```text
flux> config = {
....> host: "local",
....> port: 8080,
....> }
{host: "local", port: 8080}
flux> config.host
"local"
```

By default, runtime execution installs the current builtin prelude. Scalar snippets still print compact values, while query-style scripts now render named result blocks and simple terminal tables. Use `--list-results` to print only the available named results, `--output-format human|csv|json` to switch result serialization, `--annotated-csv` as a backwards-compatible alias for CSV output, `--result <name>` to emit only one named result from a multi-result script, `--quiet` to suppress value output, or `--no-prelude` to execute only explicitly declared/imported symbols.

When a script emits multiple named results, `--result <name>` narrows output to just that one result across human, CSV, and JSON modes:

```bash
./bazel-bin/cpp/pl/flux/flux --list-results cpp/pl/flux/examples/ops_dashboard/dual_region_latest.flux
./bazel-bin/cpp/pl/flux/flux --result latest_east_mem cpp/pl/flux/examples/ops_dashboard/dual_region_latest.flux
./bazel-bin/cpp/pl/flux/flux --output-format json --result latest_west_cpu cpp/pl/flux/examples/ops_dashboard/dual_region_latest.flux
```

Output JSON:

```bash
./bazel-bin/cpp/pl/flux/flux ast --json <<'EOF'
package metrics
import "array"
config = {base with enabled: true, tags: ["cpu", "mem"]}
status = if exists config.enabled then "ok" else "missing"
EOF
```

## Editor Support

This directory now includes a project-local Vim syntax file at
[`vim/syntax/flux.vim`](./vim/syntax/flux.vim).

The syntax rules are based on the Flux subset currently implemented in
`cpp/pl/flux`, including:

- `package`, `import`, `option`, `builtin`, `testcase`, `return`
- `if` / `then` / `else`, `exists`, `and` / `or` / `not`
- attributes such as `@trace(...)`
- strings, string interpolation, regex literals, times, durations, unsigned integers
- type keywords such as `where`, `dynamic`, `vector`, `stream`
- the current builtin/query surface such as `from`, `range`, `filter`, `map`,
  `aggregateWindow`, `join`, `pivot`, `yield`, `csv.from`, and related helpers

Install it for Vim:

```bash
mkdir -p ~/.vim/syntax ~/.vim/ftdetect
cp cpp/pl/flux/vim/syntax/flux.vim ~/.vim/syntax/flux.vim
printf 'au BufRead,BufNewFile *.flux setfiletype flux\n' > ~/.vim/ftdetect/flux.vim
```

Install it for Neovim:

```bash
mkdir -p ~/.config/nvim/syntax ~/.config/nvim/ftdetect
cp cpp/pl/flux/vim/syntax/flux.vim ~/.config/nvim/syntax/flux.vim
printf 'au BufRead,BufNewFile *.flux setfiletype flux\n' > ~/.config/nvim/ftdetect/flux.vim
```

If you prefer not to copy files, you can also add this repository's
`cpp/pl/flux/vim` directory to your `runtimepath` and register `*.flux` as the
`flux` filetype in your editor config.

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
- realistic `filter` / `map` / `aggregateWindow` / `join` / `pivot` / `union` / `reduce` query shapes
- runtime value creation, deep equality, and object lookup
- runtime environment scope chaining and option lookup
- first-pass expression evaluation for literals, identifiers, arrays/objects, member/index access, unary/binary/logical operators, conditionals, string interpolation, record update, function values, and function calls
- first-pass statement execution for variable assignment, `option` assignment, expression statements, and block/return behavior
- `testcase` execution in an isolated child scope, with per-test results exposed through `__flux.testcase.<name>`
- file-level execution across shared top-level environment state
- runtime handling for top-level `builtin` declarations
- package/import metadata handling during file execution
- command-line execution through `flux`, including `-e`, file mode, and a small REPL
- in-memory query-style execution for `from |> range |> filter |> map`, plus `limit`, `tail`, `keep`, `drop`, `rename`, `duplicate`, `set`, `reduce`, `sort`, `group`, `pivot`, `fill`, `elapsed`, `difference`, `derivative`, `count`, `first`, `last`, `union`, a lightweight two-table `join`, first-pass `aggregateWindow`, and Flux-style CSV input through `csv.from`
- tree dump / JSON dump

## Runtime Status

The runtime layer is still early, but it now has several concrete building blocks:

- `runtime_value`: runtime scalar/container values such as null, bool, int, uint, float, string, duration, time, regex, array, object, and a lightweight in-memory table value
- `runtime_env`: lexical environments with parent scopes, variable bindings, option bindings, and nearest-scope assignment
- `runtime_builtin`: a small builtin registry with `len`, `string`, `contains`, `sum`, `mean`, `min`, `max`, first-pass query builtins `from`, `range`, `filter`, `map`, `limit`, `tail`, `keep`, `drop`, `rename`, `duplicate`, `set`, `reduce`, `sort`, `group`, `pivot`, `fill`, `elapsed`, `difference`, `derivative`, `distinct`, `count`, `first`, `last`, `union`, `join`, and `aggregateWindow`, plus an imported `csv.from` package builtin
- `runtime_builtin`: a small builtin registry with `len`, `string`, `contains`, `sum`, `mean`, `min`, `max`, first-pass query builtins `from`, `range`, `filter`, `map`, `limit`, `tail`, `keep`, `drop`, `rename`, `duplicate`, `set`, `reduce`, `sort`, `group`, `pivot`, `fill`, `elapsed`, `difference`, `derivative`, `distinct`, `count`, `first`, `last`, `union`, `join`, `aggregateWindow`, and a first-pass `yield`, plus an imported `csv.from` package builtin
- `runtime_eval`: a first expression evaluator for AST expressions, including function values and function calls
- `runtime_exec`: a first statement executor for assignments, `option`, expression statements, and block/return control flow
- `flux_cli`: a small CLI/REPL wrapper around the parser and runtime executor

File execution keeps an internal ordered list of named results for top-level statements such as assignments, expressions, options, and testcases. The CLI now uses that result list for human-readable blocks plus `csv` and `json` output modes, which gives us a workable bridge toward more official Flux/Influx result-set formatting and easier scripting.

Result naming can now also come from Flux itself through a first-pass `yield(name: "...")` builtin in query pipelines. The current implementation preserves the input table and attaches the yielded name to downstream CLI/CSV output, while annotated CSV output now also preserves existing `result`/`table` columns and derives `#group` flags from runtime `_group` metadata when available. It is still a lightweight compatibility layer rather than a full official Flux result-stream engine.

The current `aggregateWindow` implementation is still intentionally lightweight, but it now covers more of the practical Flux surface: it buckets RFC3339 `_time` values by fixed durations such as `1m`, supports fixed-offset and named-zone `location` records for window boundary calculation, supports calendar `mo` and `y` windows with timezone-aware boundaries, preserves `_group`, supports `column`, `offset` for fixed-duration windows, `timeSrc`, `timeDst`, and `createEmpty: true`, and can call `mean`, `sum`, `min`, `max`, custom array functions, or the special window form of `count`. Output rows now also drop non-group-key columns outside the aggregate target and window metadata, which keeps the shape closer to Flux `aggregateWindow()` output. Global `option location`, `period`, and calendar-window `offset` semantics are still not implemented.

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
- lightweight in-memory query pipelines using `from`, `range`, `filter`, `map`, `limit`, `tail`, `keep`, `drop`, `rename`, `duplicate`, `set`, `reduce`, `sort`, `group`, `pivot`, `fill`, `elapsed`, `difference`, `derivative`, `distinct`, `count`, `first`, `last`, `union`, `join`, and `aggregateWindow`

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

Annotated CSV supports the common Flux metadata rows `#datatype`, `#group`, and optional `#default`. Typed cells are materialized into runtime values for `string`, `long`, `unsignedLong`, `double`, `boolean`, `dateTime:RFC3339`, `dateTime:RFC3339Nano`, and `duration`; group columns are exposed through a lightweight `_group` object on each row, and repeated metadata/header blocks in one payload are accepted:

```flux
import "csv"

data = csv.from(csv: "#datatype,string,long,dateTime:RFC3339,string,double,boolean\n#group,false,false,true,true,false,false\n#default,_result,,,,,\n,result,table,_time,_measurement,_value,active\n,,0,2024-01-01T00:00:00Z,cpu,95.5,true\n")
```

This is still a lightweight in-memory table representation rather than a full Flux stream/table engine, so broader multi-table stream semantics and the full standard-library CSV surface are not complete yet.

## Benchmarks

We now keep a checked-in local benchmark workflow under
[benchmark/README.md](./benchmark/README.md) so we can compare behavior before
and after runtime optimizations.

Build the binary, generate synthetic annotated CSV data, and run the default
benchmark matrix:

```bash
bazel build //cpp/pl/flux:flux
python3 cpp/pl/flux/benchmark/generate_benchmark_data.py
python3 cpp/pl/flux/benchmark/run_benchmarks.py
```

The current baseline, collected locally on 2026-04-19, looks like this:

| Case | Input | Time |
| --- | --- | ---: |
| `linear` | 100k rows | 2.0s |
| `linear` | 500k rows | 9.9s |
| `linear` | 1M rows | 19.6s |
| `sort` | 100k rows | 1.6s |
| `sort` | 500k rows | 8.4s |
| `sort` | 1M rows | 17.6s |
| `agg` | 100k rows | 9.0s |
| `agg` | 500k rows | 39.9s |
| `agg` | 1M rows | 78.7s |
| `join` | 2000 x 2000 rows | 0.85s |
| `join` | 5000 x 5000 rows | 5.0s |

Those numbers line up with the current implementation shape:

- `csv.from` reads the whole file into memory first
- tables are represented as in-memory row vectors
- operators such as `filter`, `map`, `sort`, `group`, `pivot`, and `union`
  frequently clone rows
- `join` is still a lightweight nested-loop two-table implementation

So for now this runtime is best treated as a local in-memory engine for:

- tens of MB of CSV input
- simple pipelines up to the low millions of rows
- medium-complexity pipelines in the low hundreds of thousands of rows
- small joins, not large-table joins

Current statement execution support includes:

- variable assignment
- `testcase` statement execution with isolated local bindings and a structured result object
- successful testcase results exposed as `__flux.testcase.<name>`
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
- the remaining `aggregateWindow` gaps such as global `option location`, `period`, and calendar-window `offset`
- a broader standard-library builtin catalog

## Notes

- `flux ast` prints parser errors first, then dumps the current AST, and returns a non-zero exit code.
- AST debug output now includes source locations for top-level and several nested nodes, including statements, expressions, blocks, properties, array items, dict items, type constraints, and function/type parameter nodes.
- Source locations are still not universal across every AST node, so some debug summaries remain location-free.
