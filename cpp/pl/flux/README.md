# Flux Parser Debugging

`cpp/pl/flux` now provides an AST debugging entrypoint that can print Flux programs as:

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
- string interpolation
- regex match
- record update `{base with ...}`
- array / dict / boolean / float / duration
- `if exists ... then ... else ...`
- tree dump / JSON dump

## Notes

- `ast_dump` prints parser errors first, then dumps the current AST, and returns a non-zero exit code.
- AST nodes do not store `line:col` source locations yet, so the current debug output does not include source positions.
- If we want to add source locations later, the smallest clean path is to start with `Statement` and `Expression` nodes and add `SourceLocation`.
