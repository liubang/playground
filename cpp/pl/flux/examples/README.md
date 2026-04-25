# Flux 示例集

这个目录目前包含两组示例：

- [`ops_dashboard`](./ops_dashboard/README.md)：基于仓库内置主机指标数据的真实仪表盘风格查询
- [`feature_gallery`](./feature_gallery/README.md)：覆盖当前 builtin 面和常见能力组合的聚焦示例

如果你想快速了解当前运行时“已经能跑什么”，建议先从 [`feature_gallery/README.md`](./feature_gallery/README.md) 看起。

其中 [`feature_gallery/join_package.flux`](./feature_gallery/join_package.flux) 专门覆盖显式
`import "join"` 后的 package API；[`feature_gallery/join_union_pivot.flux`](./feature_gallery/join_union_pivot.flux)
和 [`ops_dashboard/query.flux`](./ops_dashboard/query.flux) 则继续展示默认加载的 universe 顶层 `join()`。
