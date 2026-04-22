# Flux 优化记录

这份文档记录 `cpp/pl/flux` 在 2026-04-22 这一轮围绕 benchmark 做过的主要优化、验证方式和结果解读。目标不是替代 [README.md](/Volumes/workspace/liubang/playground/cpp/pl/flux/benchmark/README.md) 里的基线表，而是把“为什么会变快”以及“哪些数字该怎么理解”留下来，方便后续继续推进。

## 背景

最开始的切入点不是 benchmark，而是一轮面向功能矩阵和语义一致性的 review。那轮先修了几类 correctness 问题：

- `range()` 从字符串比较改成 RFC3339 时间比较，并修正为 stop-exclusive。
- 一元 `-` 支持 duration 字面量，比如 `range(start: -1h)`。
- `map`、`limit`、`tail`、`keep`、`drop`、`rename`、`duplicate`、`set`、`reduce` 等算子恢复按逻辑表逐表执行，不再把多表流悄悄压扁。

把这些问题补稳后，benchmark 里最重的路径就更清楚了，尤其是：

- `aggregateWindow()`
- `group()`
- `pivot()`
- `join()`

## 优化顺序

这轮优化是按“先 correctness 保底，再做低风险结构性减法”的顺序推进的。

### 1. `aggregateWindow()`

最早的 `aggregateWindow()` 热点主要有三类：

- 先把所有行扁平处理，再靠字符串 bucket key 和 group key 回拼。
- 大量构造 `window_bucket_key` / `group_key` 字符串。
- 末尾对全量 bucket 做 `stable_sort`。

后来改成了更贴近真实语义的组织方式：

- 先按逻辑表处理。
- 在逻辑表内再按 group 维护窗口状态。
- tumbling window 场景，也就是 `every == period` 且 `period` 非负时，走单窗口快路径。

中间暴露出一个真实回归：如果同一个 chunk 里包含多个 `_group`，但我们只按 chunk 做聚合，就会把不同 group 合并掉。这个问题后来修成“按 chunk 内 group 分桶”，并加了专门回归测试锁住。

### 2. `keep/drop` 和 `pivot()`

这部分主要是把明显的 per-row 线性查找去掉：

- `keep()` / `drop()` 改成预建 `unordered_set`。
- `pivot()` 改成原地 upsert 属性，避免每插入一个 pivot 列都整行对象复制。
- `pivot()` 预建 `columnKey` / `rowKey` 集合，减少字段过滤时的重复判断。

### 3. `group()`

`group()` 原来对每一行会做两遍近似相同的工作：

- 先扫描一遍构 `_group` 对象。
- 再扫描一遍把同一批列转成字符串 key。

后面把它合并成一次扫描，同时产出 grouped row 和 group key，少掉了一轮列查找和一次对象构造。

### 4. `join()`

`join()` 这轮没有做激进重写，主要做的是小步减法：

- 预建 `on` 列集合。
- 减少重复 `contains`。
- 去掉 `join_rows()` 里重复做两遍的 group props 构造。

这类改动收益不一定像 `aggregateWindow()` 那样显著，但风险很低，适合持续累积。

## benchmark 侧的变化

为了让优化过程更可见，这轮也扩展了 benchmark 本身。

新增了两个 case：

- `group`：`csv.from |> group |> count |> yield`
- `pivot`：`csv.from |> pivot |> limit |> yield`

同时新增了专门的数据输入：

- `pivot_100000.annotated.csv`
- `pivot_500000.annotated.csv`
- `pivot_1000000.annotated.csv`

这样后续再优化 `group()` 或 `pivot()`，就不用只看 `agg` 这种混合路径。

## examples 校验

这轮不只是看 benchmark 和单点单测，也把 examples 一起纳入了回归面。

新增了一个 CLI 单测，会递归执行 `cpp/pl/flux/examples` 下所有 `.flux` example，确保：

- sample 不会和当前实现悄悄漂移。
- 做性能优化时，不会因为内部重排把 examples 跑挂。

这对 `aggregateWindow()` 那次“chunk 内不同 group 被错误合并”的回归尤其有帮助，因为它很快就在 example 路径上暴露出来了。

## 如何解读当前数字

当前 benchmark runner 更适合做“本地同机前后对比”，不适合当成严格冷启动基准。

原因有两点：

- 它直接拉起本地二进制，受文件缓存和系统热缓存影响很大。
- 连续多轮执行时，第二轮及之后的数字会明显偏低。

所以更靠谱的使用方式是：

1. 在同一台机器上，尽量用一致的顺序跑修改前。
2. 记录 JSON 输出。
3. 做改动。
4. 用同样的输入和顺序跑修改后。
5. 看相对变化，不要把单次绝对值当成最终结论。

如果后面要把这套东西用于更正式的性能门槛判断，建议补两类能力：

- 冷热分离或多轮采样。
- 输出均值、中位数和方差，而不是只保留一次 `seconds`。

## 这一轮的结论

从代码层面看，这轮优化最有价值的不是某一个微调，而是建立了一个比较清晰的性能工作流：

- 先用 review 把语义风险和错误行为清掉。
- 再围绕最重路径做结构性减法。
- 同时让 benchmark 和 examples 都能反映改动是否值得、是否安全。

从工程层面看，这轮留下来的资产包括：

- 更完整的 benchmark case 集。
- 更稳定的多表流语义。
- 全量 example 执行兜底。
- 一份可以继续续写的优化记录。

## 下一步建议

- 给 benchmark 增加冷热分离或重复采样。
- 继续优化 `pivot()` 的 row identity / 列名构造，尽量减少字符串分配。
- 继续观察 `join()` 在更大 `on` 列集合和更宽 schema 下的行为，再决定是否值得做更大改造。
