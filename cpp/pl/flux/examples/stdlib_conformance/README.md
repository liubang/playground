# Flux stdlib conformance examples

这组示例偏契约测试，而不是展示场景。每个文件只覆盖一个 package 的一小组稳定行为，并由
`//cpp/pl/flux:stdlib_conformance_test` 用 JSON 输出做固定快照校验。

新增或重构 builtin 时，优先在这里加入小而确定的 package 级样例；复杂、跨 package 的叙事示例继续放在
`feature_gallery` 或 `ops_dashboard`。

当前覆盖 `array`、`csv`、`date`、`dict`、`join`、`json`、`math`、`regexp`、`runtime`、`strings`、`system`、`types` 和默认 universe builtin。`system.time()` 的输出按 RFC3339 形状匹配，不做固定时间快照。
