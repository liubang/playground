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
bazel build //cpp/pl/flux:ast_dump
```

Optional:

```bash
bazel build //cpp/pl/flux:parser_test
```

## Usage

Read from stdin:

```bash
./bazel-bin/cpp/pl/flux/ast_dump <<'EOF'
package metrics
import "array"
config = {base with enabled: true, tags: ["cpu", "mem"]}
status = if exists config.enabled then "ok" else "missing"
EOF
```

Read from a file:

```bash
./bazel-bin/cpp/pl/flux/ast_dump path/to/query.flux
```

Output JSON:

```bash
./bazel-bin/cpp/pl/flux/ast_dump --json <<'EOF'
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
- in-memory query-style execution for `from |> range |> filter |> map`
- tree dump / JSON dump

## Runtime Status

The runtime layer is still early, but it now has several concrete building blocks:

- `runtime_value`: runtime scalar/container values such as null, bool, int, uint, float, string, duration, time, regex, array, object, and a lightweight in-memory table value
- `runtime_env`: lexical environments with parent scopes, variable bindings, option bindings, and nearest-scope assignment
- `runtime_builtin`: a small builtin registry with `len`, `string`, `contains`, `sum`, `mean`, `min`, `max`, and first-pass query builtins `from`, `range`, `filter`, and `map`
- `runtime_eval`: a first expression evaluator for AST expressions, including function values and function calls
- `runtime_exec`: a first statement executor for assignments, `option`, expression statements, and block/return control flow

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
- lightweight in-memory query pipelines using `from`, `range`, `filter`, and `map`

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

Not implemented yet in the runtime:

- richer query-oriented pipe semantics beyond the current in-memory table pipeline subset
- `testcase` execution
- a broader standard-library builtin catalog

## Notes

- `ast_dump` prints parser errors first, then dumps the current AST, and returns a non-zero exit code.
- AST debug output now includes source locations for top-level and several nested nodes, including statements, expressions, blocks, properties, array items, dict items, type constraints, and function/type parameter nodes.
- Source locations are still not universal across every AST node, so some debug summaries remain location-free.
