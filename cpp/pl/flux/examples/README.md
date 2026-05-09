# Flux 示例集

这个目录目前包含四组示例：

- [`ops_dashboard`](./ops_dashboard/README.md)：基于仓库内置主机指标数据的真实仪表盘风格查询
- [`feature_gallery`](./feature_gallery/README.md)：覆盖当前 builtin 面和常见能力组合的聚焦示例
- [`cross_source`](./cross_source)：SQLite 指标表、CSV 维表和内联 array 表的跨源查询示例
- [`stdlib_conformance`](./stdlib_conformance/README.md)：每个 stdlib package 一两个固定输入/输出的小样例，由 Bazel sh_test 做 JSON 快照校验

如果你想快速了解当前运行时“已经能跑什么”，建议先从 [`feature_gallery/README.md`](./feature_gallery/README.md) 看起。

其中 [`cross_source/sqlite_pushdown_explain.flux`](./cross_source/sqlite_pushdown_explain.flux)
展示了 SQLite 源上的 `range/filter/filter/drop/rename/keep/sort/limit/distinct` 下推、连续简单
filter 的 predicate 累积、`drop()` 反向 projection、`rename()` projection alias、`distinct()`
列映射，以及
`explain()` 对 `[sqlite pushdown]`、`[sqlite scan]` 和 barrier/memory 边界的标注，
以及 `SourcePushdown(request: ...)` 物理下推摘要。

[`cross_source/sqlite_csv_array_incidents.flux`](./cross_source/sqlite_csv_array_incidents.flux)
则把 SQLite 指标聚合、CSV owner 维表和 `array.from` 阈值表串起来，展示 `sqlite.from` /
`csv.from` / `array.from` provider API 形态下的三源 join 与内存 fallback。示例集不使用
universe 顶层数据源入口。

其中 [`feature_gallery/join_package.flux`](./feature_gallery/join_package.flux) 专门覆盖显式
`import "join"` 后的 package API；[`feature_gallery/join_union_pivot.flux`](./feature_gallery/join_union_pivot.flux)
和 [`ops_dashboard/query.flux`](./ops_dashboard/query.flux) 则继续展示默认加载的 universe 顶层 `join()`。
