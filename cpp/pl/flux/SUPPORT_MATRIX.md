# Flux Parser Support Matrix

This document describes the current implementation status of `cpp/pl/flux`.

Status meanings:

- `Supported`: implemented and covered by current tests or debug tooling
- `Partial`: some common cases work, but the implementation is incomplete or fragile
- `Missing`: not implemented yet, or not reliable enough to claim support

## File-Level Syntax

| Syntax | Status | Notes | Example |
| --- | --- | --- | --- |
| `package` clause | Supported | Parsed into `PackageClause` | `package metrics` |
| `import "path"` | Supported | Alias-less import works | `import "array"` |
| `import alias "path"` | Supported | Alias is stored in AST | `import regexp "regexp"` |
| file body statements | Supported | Mixed statement list works | `a = 1` |
| attributes / annotations | Partial | Package/import/top-level statement attributes are attached to AST nodes, but broader coverage is still incomplete | `@edition("2022.1")` |

## Statements

| Syntax | Status | Notes | Example |
| --- | --- | --- | --- |
| expression statement | Supported | Parsed from top-level expressions | `from(bucket: "x")` |
| variable assignment | Supported | Main assignment path works | `status = "ok"` |
| `option name = expr` | Supported | Parsed as `OptionStatement` with variable assignment | `option task = {name: "cpu"}` |
| `option a.b = expr` | Supported | Member assignment path works | `option task.offset = 5m` |
| `return expr` | Supported | Works inside blocks | `return 1 + 2` |
| `builtin name : type` | Supported | Includes function and record type cases | `builtin sum : (a: int) => int` |
| `testcase` statement | Supported | Basic `extends` and block parsing work | `testcase t extends "base" { return 1 }` |
| invalid / bad statement recovery | Partial | `BadStmt` exists, but recovery is still coarse | invalid input |

## Expressions

| Syntax | Status | Notes | Example |
| --- | --- | --- | --- |
| identifier | Supported | `true` and `false` are special-cased into booleans | `value` |
| integer literal | Supported | Basic integer parsing works | `123` |
| float literal | Supported | Common decimal form works | `0.5` |
| string literal | Supported | Plain string parsing works | `"cpu"` |
| string interpolation | Supported | `${expr}` works inside quote-based string expressions | `"host ${user}"` |
| duration literal | Supported | Common units parse | `1h`, `5m` |
| datetime literal | Supported | RFC3339-like path works for current implementation | `2024-01-02T03:04:05Z` |
| regex literal | Supported | Regex scanning depends on expression context and is now working | `/cpu.*/` |
| boolean literal | Supported | `true` / `false` are mapped to `BooleanLit` | `true` |
| array literal | Supported | Common array forms work | `["cpu", "mem"]` |
| dictionary literal | Supported | Key/value dictionary parsing works | `["cpu": 1, "mem": 2]` |
| object literal | Supported | Standard object properties work | `{name: "cpu"}` |
| record update | Supported | `with` source is supported | `{base with enabled: true}` |
| member access | Supported | Parsed as `MemberExpr` | `config.enabled` |
| index access | Supported | Numeric index works; string index on objects is normalized into member-style access | `arr[0]`, `obj["enabled"]` |
| unary expression | Supported | Includes `exists`, unary `-`, `not` path | `exists config.enabled` |
| binary expression | Supported | Common arithmetic and comparison operators work | `a + b`, `x =~ /cpu.*/` |
| logical expression | Supported | `and` / `or` work on common cases | `a and b` |
| conditional expression | Supported | `if ... then ... else ...` works | `if exists x then "ok" else "bad"` |
| call expression | Supported | Common call syntax works | `range(start: -1h)` |
| function expression | Supported | Arrow function expressions work | `(r) => r.host == "local"` |
| pipe expression | Supported | Multi-stage pipe chains work | `from(...) |> range(...)` |
| label literal | Supported | Top-level label expressions now parse through the normal expression-statement path | `.field` |
| paren expression | Supported | Simple grouping works | `(1 + 2)` |
| unsigned integer literal | Supported | `123u`-style literals are scanned and parsed into `UnsignedIntegerLit` | `42u` |

## Type Syntax

| Syntax | Status | Notes | Example |
| --- | --- | --- | --- |
| basic type | Supported | Named types such as `int`, `string`, `bool` work | `int` |
| type variable | Partial | `Tvar` structure exists, but broader generic behavior is still limited | `A` |
| array type | Supported | Works via `[type]` | `[int]` |
| dict type | Supported | Works via `[key:value]` | `[string:int]` |
| record type | Supported | Common property form works | `{name: string, value: int}` |
| record type with `with` source | Supported | Current parser handles this basic pattern | `{A with name: string}` |
| function type | Supported | Required, optional, and pipe params work in common cases | `(<-tables: [int], ?n: int) => int` |
| dynamic type | Partial | Placeholder implementation exists | `dynamic`-style path |
| vector / stream type | Partial | AST branches exist; broader coverage is still incomplete | `vector[int]`, stream forms |
| `where` constraints | Supported | Single and comma-separated basic constraints are parsed | `where A: Addable` |

## Debugging and Tooling

| Capability | Status | Notes |
| --- | --- | --- |
| AST tree dump | Supported | `dump_ast(const File&)` and `ast_dump` default output |
| AST JSON dump | Supported | `dump_ast_json(const File&)` and `ast_dump --json` |
| command-line AST dumper | Supported | `bazel build //cpp/pl/flux:ast_dump` |
| parser demo binary | Supported | `parser_test` now uses the tree dump |
| parser unit tests | Supported | Covers main happy paths and dump output |
| scanner unit tests | Supported | Covers comments, regex mode, and unread behavior |
| AST source locations | Missing | Tokens have positions, but AST nodes do not store them yet |

## Error Handling

| Capability | Status | Notes |
| --- | --- | --- |
| parser error collection | Supported | `Parser::errors()` exposes collected parser errors |
| continue after local parse failures | Partial | Some recovery exists via `BadExpr` / `BadStmt`, but it is not robust |
| precise syntax diagnostics | Partial | Some messages are useful, but consistency is still limited |
| source-positioned AST errors | Missing | Requires `SourceLocation` on AST nodes |

## Current Strengths

- Common Flux file structure can already be parsed into AST
- Pipe-heavy query shapes are working
- Type parsing is good enough for `builtin` declarations and debugging
- AST tree and JSON output make parser behavior inspectable
- There is now enough unit-test coverage to safely extend the parser

## Main Known Gaps

- AST nodes do not carry source locations yet
- Some syntax families are only partially implemented
- Error recovery is still weak for malformed programs
- A few AST/debug string forms are still simplified rather than canonical Flux formatting
- There is no semantic analysis or type checking layer yet

## Recommended Next Steps

1. Add `SourceLocation` to `Statement` and `Expression`
2. Expand partial syntax areas into tested supported features
3. Improve malformed-input recovery and diagnostics
4. Add more negative tests and dump snapshots
5. Separate parser completeness from future semantic/type-check phases

## Partial Feature Examples

These examples are useful as the next wave of parser tests because they sit right on the current boundary between working and incomplete behavior.

### Attributes / annotations

Example:

```flux
@edition("2022.1")
package metrics
```

Current expectation:

- package/import/top-level statement attributes are attached to AST nodes
- tree dump output includes attached attributes
- broader node coverage is still incomplete, so this stays `Partial`

### Label literal

Example:

```flux
.field
```

Current expectation:

- `LabelLit` exists in the AST
- real-world coverage is still thin
- needs a parser test to confirm where it is valid and how it interacts with member access

### Unsigned integer literal

Example:

```flux
42u
```

Current expectation:

- scanned as a dedicated unsigned integer token
- parsed into `UnsignedIntegerLit`
- covered by scanner and parser unit tests

### Dynamic type

Example:

```flux
builtin x : dynamic
```

Current expectation:

- parser has a placeholder dynamic type path
- behavior should be validated with a dedicated builtin/type-expression test

### Vector / stream type

Examples:

```flux
builtin x : vector[int]
builtin y : stream[int]
```

Current expectation:

- AST branches exist
- parser behavior has not been fully covered with tests
- these are good candidates for the next type-system expansion pass
