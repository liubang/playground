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
| attributes / annotations | Supported | Package/import/statement attributes are attached to AST nodes, including block statements, and attribute parameters accept full expressions | `@edition("2022.1")` |

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
| function expression | Supported | Parenthesized params, shorthand single-param arrows, block bodies, pipe params, and optional/default params are covered by tests | `(r) => r.host == "local"` |
| pipe expression | Supported | Multi-stage pipe chains work | `from(...) |> range(...)` |
| label literal | Supported | Top-level label expressions now parse through the normal expression-statement path | `.field` |
| paren expression | Supported | Simple grouping works | `(1 + 2)` |
| unsigned integer literal | Supported | `123u`-style literals are scanned and parsed into `UnsignedIntegerLit` | `42u` |

## Type Syntax

| Syntax | Status | Notes | Example |
| --- | --- | --- | --- |
| basic type | Supported | Named types such as `int`, `string`, `bool` work | `int` |
| type variable | Supported | Single-token type variables are parsed in type expressions and constraints | `A` |
| array type | Supported | Works via `[type]` | `[int]` |
| dict type | Supported | Works via `[key:value]` | `[string:int]` |
| record type | Supported | Common property form works | `{name: string, value: int}` |
| record type with `with` source | Supported | Current parser handles this basic pattern | `{A with name: string}` |
| function type | Supported | Required, optional, and pipe params work in common cases | `(<-tables: [int], ?n: int) => int` |
| dynamic type | Supported | Parsed into `Dynamic` monotype and exposed through AST/debug output | `dynamic` |
| vector / stream type | Supported | Parsed into dedicated monotypes and covered by parser tests, including malformed recovery | `vector[int]`, `stream[int]` |
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
| AST source locations | Supported | Top-level file/package/import/statement/expression/block nodes store locations and dump them | `loc=1:1-1:10` |

## Runtime Foundations

| Capability | Status | Notes |
| --- | --- | --- |
| runtime value model | Supported | Supports null/bool/int/uint/float/string/duration/time/regex/array/object values plus a lightweight in-memory table value |
| lexical environments | Supported | Parent scopes, variable bindings, option bindings, and nearest-scope assignment are covered by unit tests |
| expression evaluator | Supported | Supports literals, identifiers, arrays/objects, record update, member/index access, unary/binary/logical operators, conditionals, string interpolation, regex match, function values, function calls, simple pipe forwarding, and in-memory `from`/`range`/`filter`/`map` query execution |
| statement execution | Partial | Supports variable assignment, `option` assignment, expression statements, block/return execution, file-level sequential execution, top-level `builtin` declarations, package/import metadata handling, and end-to-end execution of simple in-memory query files; `testcase` execution is still missing |
| function values / closures | Supported | User-defined function expressions now evaluate to callable runtime values with lexical closure capture |
| function call execution | Supported | Builtin and user-defined function calls work, including default arguments, named arguments, block-bodied functions, pipe-parameter injection, and internal row-function invocation for in-memory query builtins |
| pipe execution | Partial | Value forwarding through `|>` works for builtin functions, user-defined `<-pipe` parameters, and lightweight in-memory table pipelines, but broader query/stream semantics are still missing |
| builtin registry / stdlib execution | Partial | A small callable builtin registry exists today (`len`, `string`, `contains`, `sum`, `mean`, `min`, `max`, `from`, `range`, `filter`, `map`), top-level `builtin` declarations can bind known builtins or placeholder callables, but the Flux standard library is still largely missing |

## Error Handling

| Capability | Status | Notes |
| --- | --- | --- |
| parser error collection | Supported | `Parser::errors()` exposes collected parser errors |
| continue after local parse failures | Partial | Recovery exists for conditionals, call args, arrays, dicts, object properties, attributes, and some type syntax, but it is not yet uniform | invalid input |
| precise syntax diagnostics | Partial | Many messages are useful and now include more localized context, but consistency is still limited |
| source-positioned AST errors | Partial | Many bad expressions/statements include locations, but this is not yet universal for all parse errors |

## Current Strengths

- Common Flux file structure can already be parsed into AST
- Pipe-heavy query shapes are working
- Realistic query fragments with alias imports, `filter`/`map` chains, regex calls, record updates, and conditional expressions are covered by parser tests
- Function expressions now cover the practical forms needed by most Flux queries: `(r) => expr`, `r => expr`, `r => { ... }`, `(<-tables, ?limit=5, value) => expr`
- Empty literals and empty record updates are covered, including `[]`, `{}`, `[:]`, and `{base with}`
- Type parsing is good enough for `builtin` declarations and debugging
- AST tree and JSON output make parser behavior inspectable
- There is now enough unit-test coverage to safely extend the parser
- The runtime has a tested value model, scope chain, expression evaluator, and first-pass statement executor to build on

## Main Known Gaps

- Some syntax families are still only partially implemented or only lightly tested
- Error recovery is still uneven for malformed programs
- A few AST/debug string forms are still simplified rather than canonical Flux formatting
- There is no semantic analysis or type checking layer yet
- Runtime execution is only partial: a useful subset of statements and expressions can execute, including simple in-memory query pipelines, but broader builtins and stream semantics are still missing

## Recommended Next Steps

1. Finish parser completeness for the remaining partial areas and add more realistic end-to-end query fixtures
2. Improve malformed-input recovery and diagnostics until bad programs still produce usable AST/debug output
3. Add more negative tests and dump snapshots
4. Extend `SourceLocation` coverage beyond current top-level/core AST nodes
5. Continue the execution architecture from the current base: callable functions/builtins, then pipe/query execution

## Suggested Next Syntax Additions

These are good parser targets before starting execution work in earnest:

- More real query operators and shapes such as `join`, `union`, `aggregateWindow`, and `reduce`
- More `option`-driven query programs, especially `option task = {...}` combined with pipelines
- Broader function-body fixtures that mix block bodies, object returns, nested calls, and conditionals
- More malformed-input fixtures around complex call/object/type combinations

## Interpreter Roadmap

The long-term goal is a usable Flux interpreter with both parsing and execution. A practical sequence is:

1. Parser-complete baseline

- Keep expanding tested syntax until the common query subset is stable
- Preserve AST/debug quality so parser failures stay easy to diagnose

2. Semantic/runtime foundation

- Define runtime value types such as null/bool/int/uint/float/string/time/duration/regex/array/object/function
- Add lexical environments and scope lookup for variables, options, and function parameters
- Define callable builtins and a registry for standard library entry points

3. Expression interpreter

- Evaluate literals, arrays, objects, dictionaries, arithmetic/logical operators, member/index access, and conditionals
- Support function values, closures, and function calls
- Support `option` bindings and `builtin` declarations at runtime

Current status:

- runtime values: started
- environments/scopes: started
- expression evaluation: started
- statement execution: started
- function calls and closures: started

4. Query / pipeline execution

- Continue growing the current lightweight table model and pipe input support
- Extend beyond the current `from`, `range`, `filter`, `map` baseline to later `group`, `join`, `aggregateWindow`
- Define enough runtime behavior to run realistic Flux scripts end to end

5. Diagnostics and usability

- Add runtime errors with source locations
- Add script runner examples and interpreter-focused tests
- Keep `ast_dump` and parser diagnostics aligned with runtime debugging needs

## Partial Feature Examples

These examples are useful as the next wave of parser tests because they sit right on the current boundary between working and incomplete behavior.

### Label literal

Example:

```flux
.field
```

Current expectation:

- `LabelLit` exists in the AST
- real-world coverage is still thin
- needs a parser test to confirm where it is valid and how it interacts with member access
