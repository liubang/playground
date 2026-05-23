# Flux stdlib conformance examples

这组示例偏契约测试，而不是展示场景。每个文件只覆盖一个 package 或一组默认 universe builtin 的稳定行为，并由
`//cpp/pl/flux/cli:stdlib_conformance_test` 用 JSON 输出做快照校验。

新增或重构 builtin 时，优先在这里加入小而确定的 package 级样例；复杂、跨 package 的叙事示例继续放在
`feature_gallery` 或 `ops_dashboard`。

## 覆盖原则

- 每个实现过的 builtin 都必须在这里有一个主覆盖点。
- 一个 builtin 只在一个文件里作为主覆盖点；跨文件复用的 `findRecord`、`len` 等只作为取值辅助。
- `syntax.flux` 覆盖当前可执行的语言语法面；只放 parser 与 runtime 都能实际执行的形态。
- `system.time()` 的输出按 RFC3339 形状匹配，不做固定时间快照。

## 覆盖清单

| 文件                      | 主覆盖                                                                                                                                                                               |
| ------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `array.flux`              | `array.from`、`array.concat`、`array.filter`、`array.map`、`array.contains`、`array.reduce`、`array.any`、`array.all`                                                                |
| `csv.flux`                | `csv.from`                                                                                                                                                                           |
| `date.flux`               | `date.add`、`date.sub`、`date.truncate`、`date.year`、`date.month`、`date.monthDay`、`date.weekDay`、`date.hour`、`date.minute`、`date.second`                                       |
| `dict.flux`               | `dict.fromList`、`dict.get`、`dict.insert`、`dict.remove`                                                                                                                            |
| `join.flux`               | `join.inner`、`join.left`、`join.right`、`join.full`                                                                                                                                 |
| `json.flux`               | `json.encode`                                                                                                                                                                        |
| `math.flux`               | `math.pi`、`math.abs`、`math.ceil`、`math.floor`、`math.round`、`math.sqrt`、`math.pow`                                                                                              |
| `regexp.flux`             | `regexp.compile`、`regexp.findString`、`regexp.matchRegexpString`、`regexp.quoteMeta`                                                                                                |
| `runtime.flux`            | `runtime.version`                                                                                                                                                                    |
| `sqlite.flux`              | `sqlite.from`（SQLite table scan 物化到内存表）                                                                                                                                     |
| `strings.flux`            | `strings.containsStr`、`strings.hasPrefix`、`strings.hasSuffix`、`strings.joinStr`、`strings.replaceAll`、`strings.split`、`strings.toUpper`、`strings.toLower`、`strings.trimSpace` |
| `syntax.flux`             | `package`、alias `import`、`option`、`builtin`、attribute、`testcase`、literal、数组/字典/对象/record update、函数/闭包/默认参数/pipe 参数、位置/命名调用、函数作为参数/返回值/对象和数组成员、`reduce` 风格 Fibonacci/factorial/max、调用/管道、高阶函数、运算符、条件、`exists`、字符串插值 |
| `system.flux`             | `system.time`                                                                                                                                                                        |
| `types.flux`              | `types.isBool`、`types.isDuration`、`types.isFloat`、`types.isInt`、`types.isNumeric`、`types.isRegexp`、`types.isString`、`types.isTime`、`types.isType`、`types.isUInt`            |
| `universe_core.flux`      | `len`、`string`、`contains`                                                                                                                                                          |
| `universe_transform.flux` | `range`、`filter`、`map`、`limit`、`tail`、`keep`、`drop`、`rename`、`duplicate`、`set`、`sort`、`group`、`pivot`、`fill`、`union`                                                   |
| `universe_aggregate.flux` | `sum`、`mean`、`min`、`max`、`reduce`、`distinct`、`count`、`spread`、`quantile`、`median`、`first`、`last`、`top`、`bottom`                                                         |
| `universe_window.flux`    | `elapsed`、`difference`、`derivative`、`window`、`aggregateWindow`                                                                                                                   |
| `universe_inspect.flux`   | `columns`、`keys`、`findColumn`、`findRecord`、`explain`、`yield`                                                                                                                    |
| `universe_join.flux`      | 顶层 `join()`                                                                                                                                                                        |
