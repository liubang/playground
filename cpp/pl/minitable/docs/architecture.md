# minitable 分布式表格存储架构设计

## 1. 文档定位

本文定义 minitable 的目标架构和核心语义。它不是对当前代码的描述，而是后续实现、协议演进、正确性验证和性能验收的依据。

minitable 的目标是实现一个生产可用的分布式半结构化宽表存储，具备以下能力：

- 复合 RowKey（Composite RowKey），支持多个有类型、有顺序的 key 列；
- 稀疏、半结构化列，严格区分 NULL、空值、不存在和删除；
- ColumnFamily（CF）作为列的逻辑分组；
- LocalityGroup（LG）作为多个 CF/列的物理存储分组；
- 多版本、TTL、Put、Delete、Merge、CheckAndMutate、Increment 等常见语义；
- 可扩展并可版本化的 MergeOperator；
- Get、Seek、范围 Scan、分页 Scan、反向 Scan；
- 大范围 Scan 分批获取：GLOBAL_ORDER 使用 `next_row_key`，HASH 使用包含各 bucket 进度的 opaque token；跨 Slice 变化时不漏读、不重复；
- 每个 Slice 独立 Raft，支持强一致读写和可配置的弱一致读；
- 在线 Split、Merge、副本迁移、故障修复和自动 Balance；
- 通过 sstv2、minidfs、braft 和 brpc 构成自研、可协同演进的完整存储栈。

本文允许并要求同步修改 sstv2、minidfs 和 SkipList。它们不是不可变的外部依赖，而是 minitable 存储栈的一部分；任何不满足功能、正确性或性能目标的能力都应在对应模块中正确实现，而不是在 minitable 中增加临时绕行逻辑。

---

## 2. 设计原则

### 2.1 正确性优先

1. 已提交的 Raft entry 必须可确定性重放，apply 阶段不得依赖本地时钟、随机数或本地调度结果。
2. RPC 不得返回假成功；成功意味着操作已经达到接口承诺的持久性和可见性。
3. 所有跨线程、跨 LG、跨副本状态切换必须有明确的原子可见点。
4. 所有持久格式必须版本化；字段删除后保留编号，禁止复用。
5. 所有后台任务都必须可重入、可恢复、幂等。

### 2.2 分层但不割裂

- minitable 定义表格语义、分布式一致性、LSM 生命周期和调度策略；
- sstv2 定义高性能不可变 SST 文件、Key 编码、流式迭代器和单 SST 查询；
- minidfs 提供不可变文件的可靠存储、原子发布、校验、租约和批量 IO；
- braft 提供 Master 和 Slice 的复制日志、Leader 选举和配置变更；
- SkipList/MemTable 提供单写多读的内存有序视图。

LSM、Manifest、Compaction 属于 minitable，而不是把 sstv2 错误描述成一个完整 LSM 引擎。

### 2.3 稳定 ID 优先于名称

Table、CF、LG、Column、Slice、Replica 均使用稳定数字 ID。名称用于用户接口和展示；Rename 不改变 ID。删除后的 ID 永不复用。

### 2.4 控制面与数据面分离

Master 不在普通 point Get/Put/Delete 的数据链路中。Client 缓存路由并直连 Slice Primary/Secondary；缓存未命中或 epoch 过期时访问 Master。需要跨 Slice 一致视图的 Scan/Snapshot session 在开始时访问一次独立可水平扩展的 TSO gateway；gateway 从 Master Raft 租用 timestamp range，Master 本身仍不承载逐页数据请求。

### 2.5 有界资源

所有队列、批次、单行大小、单请求大小、Scan 页面、Merge 链长度、并发 Compaction 和迁移任务均有上限，并通过 admission control 和 backpressure 明确反馈。

---

## 3. 系统边界与组件

```text
                         ┌──────────────────────────┐
                         │      Master Raft Group   │
                         │ Schema / Route / Scheduler│
                         │ TSO / Operation Ledger   │
                         └────────────┬─────────────┘
                                      │ route/schema/command
          ┌───────────────────────────┼───────────────────────────┐
          │                           │                           │
┌─────────▼─────────┐       ┌─────────▼─────────┐       ┌─────────▼─────────┐
│   UnitServer A    │       │   UnitServer B    │       │   UnitServer C    │
│ Slice 10 Primary  │◀Raft─▶│ Slice 10 Follower │◀Raft─▶│ Slice 10 Follower │
│ Slice 11 Follower │       │ Slice 11 Primary  │       │ Slice 11 Follower │
└─────────┬─────────┘       └─────────┬─────────┘       └─────────┬─────────┘
          │                           │                           │
          └───────────────────────────┼───────────────────────────┘
                                      │ immutable SST / manifest / snapshot
                              ┌───────▼────────┐
                              │    minidfs     │
                              └────────────────┘
```

### 3.1 核心概念

| 概念 | 定义 |
|---|---|
| Cluster | 一个 Master Raft Group 和若干 UnitServer |
| Region | 资源、配额、故障域和存储根路径的逻辑隔离单元 |
| Table | Schema、分区、版本策略、CF 和 LG 的集合 |
| Row | 由复合 RowKey 唯一标识的一组稀疏 Cell |
| ColumnFamily | 列的逻辑命名、版本、TTL、权限和 Merge 策略边界 |
| Qualifier | CF 内的列标识，可为静态 Schema 列或动态 bytes |
| LocalityGroup | 一个或多个 CF 的物理存储组，拥有独立 MemTable、SST、缓存和 Compaction |
| Slice | Table 的一个水平分片和一个独立 Raft Group |
| Replica | Slice 在一个 UnitServer 上的副本 |
| Manifest | 某个 Slice/LG 当前可见 SST 集合的版本化描述 |

---

## 4. 数据模型

## 4.1 复合 RowKey

逻辑 RowKey 为：

```text
RowKey = (key_0, key_1, ..., key_n-1)
```

每个 key 列包含稳定 `column_id`、名称、标量类型和排序方向。支持：BOOL、INT32、UINT32、INT64、UINT64、FLOAT、DOUBLE、STRING、BYTES。RowKey：

- 至少包含一列；
- 不允许 NULL、ARRAY、MAP；
- 创建后不可修改列数、顺序、类型和排序方向；
- 编码必须统一调用 sstv2 memcomparable codec；
- Master、Client 和 UnitServer 不得各自实现不同编码器。

分区 key 必须是 RowKey 的非空连续前缀。路由只依据逻辑 RowKey，不受 CF、qualifier、version 影响。

## 4.2 ColumnFamily

CF 是逻辑分组，负责：

- qualifier 命名空间；
- 默认值类型或动态值约束；
- `max_versions`、`min_versions`；
- TTL；
- 默认 MergeOperator；
- Bloom 策略、缓存优先级和权限策略；
- 静态列定义与动态 qualifier 策略。

一个 CF 必须且只能属于一个 LG。一个 LG 可以包含多个 CF。移动 CF 到其他 LG 是在线数据重写操作，不是简单元数据变更。

CF 支持两类 qualifier：

1. **静态 qualifier**：Schema 中声明，具有稳定 `column_id`、名称、类型和 nullable 属性；
2. **动态 qualifier**：运行时提供 bytes 名称，类型由 CF 默认 TypeDescriptor 或每值自描述约束确定。

协议必须直接表达这一模型，不能继续使用只有 `column_id` 的扁平列引用。统一使用：

```text
ColumnFamilyDef {
  cf_id, name, locality_group_id, version_policy, ttl, merge_operator,
  dynamic_qualifier_policy, repeated StaticColumnDef
}
CellRef {
  cf_id,
  oneof { static_column_id, dynamic_qualifier_bytes }
}
CellMutation { CellRef ref, mutation_type, optional Value value }
```

公开 Data RPC、内部 RowTransaction、Snapshot 和 Schema 都复用同一 `CellRef` 语义；内部持久化前再将其 canonicalize 为 `CfId + QualifierToken`。当前 `proto/v2` 中仅含 `column_id` 的结构必须据此升级。

## 4.3 LocalityGroup

LG 是物理隔离边界，每个 Slice 的每个 LG 拥有：

- Active MemTable；
- 0 或 1 个 Immutable MemTable；
- 独立 Block Cache；
- 独立 SST Levels 和 Manifest；
- Flush/Compaction 队列；
- 压缩、block size、target file size、cache、TTL 回收策略。

典型配置：

- `hot`：高缓存、较小 block、低压缩延迟；
- `cold`：Zstd、大 block、低缓存；
- `blob`：较低 value separation threshold，大对象更多写入 value file。

跨 LG 的单行写由一条 Slice Raft entry 表达，并具有原子可见性。

## 4.4 Cell 与值语义

逻辑 Cell 标识：

```text
CellKey = (RowKey, cf_id, qualifier)
CellVersion = (CellKey, commit_ts)
```

值支持标量、ARRAY、MAP 和 NULL。必须严格区分：

| 状态 | 含义 |
|---|---|
| 不存在 | 从未写入，或已被 tombstone 屏蔽 |
| NULL | Cell 存在，值为 NULL |
| 空值 | 空 STRING/BYTES/ARRAY/MAP，是合法值 |
| Tombstone | 删除屏障，不是 Value |

ARRAY/MAP 可异构，但允许 Schema 通过递归 TypeDescriptor 限制元素、key 和 value 类型。MAP 在持久化前按 sstv2 Value 比较顺序 canonicalize，重复 key 非法。

## 4.5 多版本与 MVCC

每个写事务获得全局可比较的 `commit_ts`。Timestamp Oracle（TSO）是 Master Raft 状态的一部分，分配 128-bit 逻辑时间 `(epoch, counter)`；协议可压缩编码，但比较规则必须稳定。为避免每次写访问 Master，TSO 向 Slice Primary 租用不重叠的 counter range：

```text
TimestampLease {
  tso_epoch
  lease_id
  owner_slice_id
  owner_raft_term
  start_counter
  end_counter          // exclusive
  expires_at_physical_ms
}
```

约束：

- lease 只有在 Master Raft commit 后生效；
- 不同 lease 的区间永不重叠，已分配区间永不复用；
- Slice Leader 仅在 owner term 匹配且 lease 未过期时分配时间戳；
- Leader 切换后，新 Leader 必须取得新 lease，不能继承旧进程未持久化的 cursor；
- TTL 使用 commit_ts 中受控的 physical component 或单独持久化的 commit physical time，不用本地读取时钟推导历史版本；
- 同一事务内所有 Cell 共享 `commit_ts`，通过稳定 `mutation_seq` 确定同事务顺序。

Snapshot read 先向 TSO 获取 `read_ts`。每个 Replica 维护单调不回退的 `safe_ts`：只有已 apply 的日志、已安装的 Snapshot 和所有早于该时间戳的 mutation 都已可见时，才推进 safe_ts。Replica 仅在 `safe_ts >= read_ts` 时服务该 snapshot；否则等待、转发 Primary 或返回可重试错误。

`gc_safe_point` 由活跃 scan/read lease 的最小 read_ts、备份保留点和配置保留窗口共同决定并通过 Master Raft 发布。任何低于 safe point 的新读取被拒绝；TTL、tombstone 和 `max_versions` 仅能回收 `commit_ts < gc_safe_point` 且不再被保留策略需要的数据。read lease 超时后长扫描返回 `SNAPSHOT_EXPIRED`，不得切换 read_ts。

## 4.6 内部 Key Layout

每个 LG 使用一个 sstv2 Schema。其 user RowKey 为：

```text
StorageKey = LogicalRowKey
           + RecordPrefix
           + CfId
           + QualifierToken
```

建议编码：

```text
RecordPrefix:
  0x00 = row tombstone marker
  0x01 = CF tombstone marker
  0x02 = ordinary cell

QualifierToken:
  0x00 + ordered-varint(column_id) = static qualifier
  0x01 + escaped(raw qualifier)    = dynamic qualifier
```

`row tombstone` 使用保留 CF ID 和空 qualifier；`CF tombstone` 使用目标 CF ID 和空 qualifier。用户 CF ID 从 1 开始，0 永久保留。

最终 sstv2 `AllKey`：

```text
AllKey = StorageKey + Version(commit_ts, mutation_seq) + OpType
```

Value：

- Put：typed sstv2 Value；
- Delete：NULL value；`OpType::Delete` 表达删除操作，`RecordPrefix` 只表达删除作用域（row/CF/cell），两者职责不重叠；
- Merge：Binary `MergeOperandEnvelope`，包含 operator ID、operator version、operand type 和 payload。

该 Cell-per-entry 布局优先优化稀疏列、投影、多版本和动态 qualifier，而不是把整行序列化成大 blob。

---

## 5. Schema 与演进

## 5.1 允许的在线变更

- 新增 CF、LG、静态列；
- Rename CF、LG、静态列；
- 逻辑删除静态列或 CF；
- 调整 TTL、版本数、缓存和 Compaction 参数；
- 注册新的 MergeOperator 版本；
- 通过后台 Rewrite 将 CF 移动到新 LG。

## 5.2 禁止原地变更

- RowKey 数量、类型、顺序、排序方向；
- 分区 key；
- 已有列的不兼容类型；
- 已持久化 Merge 链的 operator ID 或不兼容 operator version；
- ID 复用。

不兼容变更通过新表和在线迁移完成。

## 5.3 Schema Version

每次 DDL 将 `schema_version` 单调加一。Data 请求携带客户端认知版本：

- 完全兼容的旧版本请求可继续服务；
- 影响目标列解释的旧版本请求返回 `SCHEMA_VERSION_STALE`；
- UnitServer 必须在应用新 Schema fence 后才能接受该版本写入；
- Schema 元数据和 operator descriptor 必须进入 Snapshot。

---

## 6. 写入语义

## 6.1 Put

Put 对单 RowKey 的多个 Cell 执行原子 mutation，可跨 CF 和 LG：

```text
validate schema/route/size
  -> admission + reserve memory
  -> allocate commit_ts
  -> canonicalize CellRef（cf_id + static column ID/dynamic qualifier）并按 LG 分组
  -> append one Slice Raft entry
  -> quorum commit
  -> deterministic apply to every LG MemTable
  -> publish visible_applied_index
  -> ACK
```

已提交 apply 不允许出现可恢复业务错误。类型校验、内存预算和 operator 校验必须在 append 前完成。apply 内部通过两阶段内存发布保证跨 LG 原子可见：

1. prepare：为全部 patch 预留 arena 空间并构造带同一 `apply_index` 的不可见节点；
2. publish：全部节点链接成功后，以 release store 一次推进 Slice `visible_applied_index`；
3. 读者在请求开始时 acquire load 可见水位，只观察 `apply_index <= read_visible_index` 的节点。

prepare 状态只存在于进程内，不写入独立持久状态。若进程在 publish 前崩溃，所有内存节点随进程消失，恢复时从 Snapshot 后的 Raft log 重新确定性构造；若 publish 后崩溃，同样由 Snapshot+日志恢复，因此不存在需要跨重启清理的半事务。Active MemTable 不允许并发 remove，保证不可见节点不会被错误回收。

若 committed apply 因不变量破坏而失败，应将副本置为 fatal/corrupt 并通过 Snapshot 修复，禁止部分成功后继续服务。

## 6.2 Delete

支持：

- DeleteCell：删除一个 qualifier；
- DeleteFamily：删除一行中的一个 CF；
- DeleteRow：删除整行；
- DeleteVersion：精确删除指定版本，可选高级接口；
- DeleteRange：管理接口，仅通过后台 range tombstone 和 Compaction 执行。

Row/CF Delete 在每个受影响 LG 写 marker。后续更高时间戳 Put 可重新创建 Cell。

## 6.3 CheckAndMutate

CheckAndMutate 的条件判断本身在 Raft apply 顺序中执行，而不是在 append 前读取后仅记录摘要：

1. Primary 将 canonical `Condition + mutation` 作为一条 entry 追加；
2. 所有副本按日志顺序在 apply 时读取 entry 之前的可见行状态并执行同一条件；
3. 条件满足时应用 mutation 并返回 `matched=true`；不满足时记录幂等结果但不产生 Cell 版本；
4. Leader closure 仅在 apply 后返回结果。

Condition 必须完整持久化，包括 `CellRef`、存在/不存在/NULL 模式、typed operand、比较操作、版本或时间范围；禁止依赖字符串表达式、非确定性 collation 或摘要还原。这样并发写由 Raft 顺序自然串行化，无需跨 append 窗口持有 row latch。实现可用 row latch 优化本地 prepare，但不能改变上述线性化点。

原子性只覆盖单 RowKey。跨行事务不作为基础能力，避免引入分布式事务复杂度。

## 6.4 Batch

- BatchGet：每行独立结果，可跨 Slice 并行；
- BatchWrite：Client 按 Slice 分组；同一 RowKey 原子，不承诺跨 RowKey 原子；
- 同 Slice 可使用 Raft group commit 降低同步开销；
- 响应逐项返回状态，不以部分成功冒充整体成功。

## 6.5 幂等

所有 mutation 必须携带 `(client_id, request_id, payload_hash)`：

- 同 ID、同 hash：返回首次提交结果和相同 commit_ts；
- 同 ID、不同 hash：返回 `IDEMPOTENCY_CONFLICT`；
- 去重记录随 Raft apply，并进入 Snapshot；
- 去重保留窗口不得短于客户端最大重试窗口；
- Increment 和非幂等 Merge 必须依赖该机制。

---

## 7. MergeOperator

## 7.1 接口

MergeOperator 由稳定 `(operator_id, operator_version)` 标识：

```text
validate_operand(schema, operand) -> Status
partial_merge(left_operand, right_operand) -> optional operand
full_merge(optional base_value, ordered operands) -> value
identity() -> optional value
```

实现必须是纯函数、确定性、类型稳定，不得访问时钟、网络或随机数。

## 7.2 代数约束

- Associative：必须；Compaction 可改变分组方式；
- Commutative：可选；若不满足，必须按 commit_ts 顺序合并；
- Identity：可选；无 base value 时决定是否能物化；
- Idempotent：可选，不能假设所有 operator 幂等；
- operator version 必须声明向后兼容范围；
- 同一个 `(CellKey, commit_ts, mutation_seq)` 只能有一个 operation；
- operands 的全序固定为 `(commit_ts ascending, mutation_seq ascending)`，非交换 operator 严格按此顺序执行；
- operator ID 或不兼容 version 边界前必须 materialize 为 Put，Compaction 不得跨边界 partial merge；
- Snapshot/Manifest 记录所需 operator 版本，缺少实现的副本不得进入 Serving。

内置 operator 建议包括：

- `counter_add_int64_v1`；
- `max_numeric_v1`、`min_numeric_v1`；
- `append_bytes_v1`；
- `set_union_v1`；
- `map_patch_v1`。

## 7.3 读取与 Compaction

读取按新到旧处理：

- 收集 Merge operands；
- 遇到 Put 作为 base，执行 full merge；
- 遇到 Delete 作为屏障；
- 无 base 且 operator 有 identity 时使用 identity；
- 无 identity 时返回明确定义的 `MERGE_BASE_MISSING` 或保留链，不能猜测默认值。

Compaction 可执行 partial/full merge，但不得跨活跃 snapshot、tombstone 或 operator version 边界错误折叠。设置 `max_merge_operands`，超过阈值触发高优先级 Compaction；读路径也有最大解析预算。

---

## 8. 读、Seek 与 Scan

## 8.1 Get

Get 支持：

- CF/qualifier/column ID 投影；
- `max_versions`；
- commit_ts 时间范围；
- strong、snapshot、bounded-staleness、eventual；
- 返回 Cell 级状态、版本和值；
- 不存在与 NULL 分离。

查询顺序为 Active MemTable、Immutable MemTable、L0 新到旧、Ln 按 key range。Bloom 和 zone map 在适用时裁剪 SST。

## 8.2 Seek

底层统一迭代器接口：

```text
SeekToFirst()
Seek(encoded_key)       // first key >= target
SeekForPrev(encoded_key)
Next()
Prev()
Valid()
key()
value()                 // lazy materialization
status()
```

MemTable、单 SST 和多层 MergeIterator 必须实现相同抽象。Seek 不得一次性 materialize 整个范围。

## 8.3 Scan

Scan 支持：

- `[start_row_key, end_row_key)`；
- 正向/反向；
- CF/列投影；
- version/time range；
- qualifier prefix；
- predicates 和 server-side filter；
- row limit、cell limit、byte limit、deadline；
- snapshot timestamp。

执行器对 MemTables 和 SST iterators 做 k-way merge，按 `StorageKey + Version` 归并，应用 row/CF/cell tombstone、TTL、版本裁剪和 MergeOperator。

Predicate pushdown 分级：

1. Key range/index 裁剪；
2. Bloom/zone map 裁剪；
3. 只解码 predicate 列；
4. 命中后再延迟加载投影 value。

## 8.4 分页和 continuation

### GLOBAL_ORDER 表

每页只在完整 Row 边界结束。响应返回：

```text
rows
next_row_key       // 最后一行之后的第一候选 key；下一页以 exclusive 方式继续
has_more
read_ts
served_route_epoch
```

下一页请求必须携带原 `read_ts`，并以 `next_row_key` 作为 exclusive lower bound。Client 可在每页前刷新路由，因此即使发生 Split/Merge，也能从逻辑 RowKey 继续：

- 不重复：下一页排除上一页最后一行；
- 不漏读：新路由从同一个逻辑 key 定位；
- 视图稳定：所有 Slice 在同一 read_ts 读取；
- 若 read_ts 已低于 GC safe point，返回 snapshot expired，而不是静默切换到新视图。

对于超过单页 byte limit 的超大行，响应额外返回 cell continuation cursor；默认配置应限制单行大小，使常规 Scan 始终按 Row 分页。

大范围 Scan 的 Client 流程：

1. 从 Master 获取当前 range routes；
2. 申请/确定 read_ts；
3. 按 Slice 顺序 Scan；
4. 每页使用 `next_row_key` 继续；
5. 收到 `SLICE_MOVED/ROUTE_EPOCH_STALE` 后刷新路由，从相同 exclusive key 重试；
6. 到达 end key 后结束。

### HASH 表

HASH 表不存在全局 RowKey 顺序，单个 `next_row_key` 无法表达多个 bucket 的扫描进度。因此 HASH Scan 不承诺全局排序，使用 opaque `scan_token`，其内部包含：

```text
read_ts
route_epoch_at_start
query_digest
per_bucket { bucket_range, next_row_key, finished }
expiry_and_signature
```

Server/Client 可并行推进多个 bucket；每个 bucket 内仍使用 `next_row_key` exclusive 续扫。路由变化后按 virtual bucket 身份重映射游标，而不是按旧 slice ID 继续。若调用方要求全局有序结果，必须选择 GLOBAL_ORDER 表，或在客户端完整收集后排序。

---

## 9. MemTable 与 LSM

## 9.1 MemTable

采用单 Raft apply writer、多 reader 模型：

- Active MemTable 仅插入，不执行在线物理删除；
- 达到阈值后原子 freeze 为 Immutable；
- 新写进入新 Active；
- reader 持有 immutable shared ownership；
- Flush 完成并无 reader 引用后释放；
- 不调用现有 SkipList 的并发不安全 `remove()`。

需要为 MemTable 增加 arena 分配、版本可见性、迭代器 pin 和精确内存记账。若现有 SkipList 无法高效满足，应改造它，而不是在上层增加全局读锁。

## 9.2 Levels

建议：

- L0：允许 key range 重叠，新文件优先；
- L1+：同层 SST key range 不重叠；
- leveled compaction 为默认；
- 大 value/冷数据允许配置 tiered 策略；
- 每个 LG 独立调度，但共享 Slice IO 和 CPU 配额。

## 9.3 Manifest

Manifest 是 minitable 的持久真相，记录：

- generation、parent generation；
- schema version；
- 每层 SST 列表；
- file ID、路径、size、checksum、min/max key；
- created/flushed applied index；
- Compaction input/output；
- GC candidate。

Raft-applied `manifest_generation` 是唯一真相。Manifest 对象文件不可变；本地或 minidfs 的 `CURRENT` 只是不具权威性的加速指针，可丢失、可重建。Flush/Compaction 先完成 SST 和 Manifest 对象，再提交包含 generation、parent generation 和对象 checksum 的 Raft `ManifestEdit`；只有该 entry apply 后 SST 才进入读路径。恢复时若 `CURRENT` 与 Snapshot/Raft generation 冲突，以 Snapshot+Raft 为准并重写 CURRENT，禁止形成双真相。

## 9.4 Flush

1. 追加/确定 flush fence index；
2. freeze MemTable；
3. 使用 sstv2 streaming Builder 构建 staging SST；
4. 写入 minidfs，校验长度和 checksum；
5. 原子 finalize 文件；
6. 通过 Slice Raft 提交 ManifestEdit；
7. apply 后发布新 Version；
8. 满足所有 LG flush watermark 后才允许截断对应 Raft log/Snapshot；
9. 异步清理 orphan staging 文件。

## 9.5 Compaction

Compaction 负责：

- 多 SST 归并；
- tombstone 和过期版本回收；
- TTL；
- Merge 链折叠；
- 大小和层级整形；
- 生成新 SST 并通过 Raft ManifestEdit 原子替换输入文件。

选择策略综合 size ratio、overlap、read amplification、tombstone ratio、merge debt 和热点。Compaction 不得阻塞 Raft apply，并受全局 IO scheduler 限流。

---

## 10. 持久化、Snapshot 与恢复

## 10.1 持久性模型

- Raft log 是未 Flush 数据的 WAL；
- minidfs 中已 finalize 且被 committed Manifest 引用的 SST 是长期数据；
- Snapshot 保存 Slice 元信息、Manifest generation、dedupe 和 operator/schema；
- 只有 `snapshot_index <= min(flushed_applied_index of all LGs)` 时才可截断日志。

## 10.2 恢复

```text
load Snapshot
  -> validate format/checksum/schema/operator availability
  -> open Manifest and referenced SST metadata
  -> replay Raft entries after snapshot index
  -> rebuild Active MemTables and dedupe tail
  -> validate visible_applied_index
  -> become ready
```

缺文件、checksum 错误或 operator 缺失时副本不得进入 Serving，必须从健康副本重新安装 Snapshot。

## 10.3 文件 GC

Live set 包括：

- 当前 Manifest；
- 活跃/保留 Snapshot；
- 正在执行的 Compaction/Flush；
- Split/Merge bootstrap；
- 安全时间窗口内的历史 generation。

文件先 MARK，再等待 generation/index/time 三重安全水位后删除。删除操作幂等。

---

## 11. 分区与路由

## 11.1 GLOBAL_ORDER

按 memcomparable RowKey range 分片，Slice 区间统一为 `[start_key, end_key)`，空边界表示无穷。适合范围 Scan 和自然热点拆分。

路由必须满足：排序、无重叠、无空洞、覆盖全集。

## 11.2 HASH

不使用 `std::hash`，也不直接绑定 `hash % physical_slice_count`。采用固定版本算法和虚拟 bucket：

```text
bucket = xxh3_64_v1(encoded_partition_key) % virtual_bucket_count
bucket range -> Slice
```

virtual bucket 数创建后不变，物理 Slice 承载连续 bucket range。Split/Merge 只移动 bucket range，避免扩容时全量 rehash。

HASH 表不支持按原始 RowKey 的全局有序 Scan；Scan 需并行扫描各 bucket/Slice，结果只保证每个 Slice 内有序。若用户需要全局 key 顺序，应使用 GLOBAL_ORDER。

## 11.3 Route Epoch

每次 Split、Merge、迁移切主或 replica route 变化使 `route_epoch` 单调增加。Data 请求携带 table ID、slice ID、schema version、route epoch。旧请求得到结构化 hint，不能被错误路由到其他数据。

---

## 12. 一致性与 Raft

每个 Slice 是独立 Raft Group：

- Primary 对应 Raft Leader；
- 写在多数派 commit 后成功；
- strong read 使用 ReadIndex，确认 Leader 权限和 applied index；
- lease read 仅作为可证明安全的优化；
- bounded-staleness read 要求 follower safe time/lag 满足限制；
- eventual read 可直接读 follower 已应用状态。

配置变更使用 joint consensus。新副本先作为 learner 追赶，达到 lag 阈值后提升 voter。

### 12.1 Fencing 层级

| Fence | 作用 |
|---|---|
| Master term | Master Leader 权限 |
| registration epoch | UnitServer 进程实例 |
| command sequence | Master 命令顺序与幂等 |
| schema version | DDL 解释 |
| route epoch | 路由与拓扑 |
| Slice Raft term/index | Slice Leader 和日志顺序 |
| Manifest generation | SST 可见集合 |

---

## 13. 在线 Split

Split 状态：

```text
ACTIVE -> PREPARING -> COPYING -> CATCHING_UP -> CUTOVER
       -> REDIRECTING -> TOMBSTONED -> GC
```

流程：

1. Scheduler 创建持久化 `operation_id`。GLOBAL_ORDER 根据 size/QPS/采样选择 `split_key`；HASH 从当前连续 virtual bucket range 中选择 `split_bucket_boundary`；
2. 父 Slice Raft 提交 `PrepareSplit`，记录 partition 类型、对应边界、child IDs 和 fence index；
3. 以 fence index 创建两个 child bootstrap snapshot。GLOBAL_ORDER 按 encoded RowKey 边界过滤，HASH 按持久化 bucket ID 过滤；
4. 父 Slice 在 COPYING 期间继续服务，并将 fence 后 mutation 按父 Raft `source_index` 可靠复制到对应 child catch-up stream；每个 child 持久化连续 `acked_source_index`，重复 entry 按 `(operation_id, source_index)` 去重，检测到空洞立即停止追赶；
5. child 建立 Raft Group、安装 Snapshot、从 fence index + 1 追平 mutation；追赶期间父 Slice 为该 operation 注册 `cutover_gc_fence`，禁止 Compaction/TTL/版本 GC 回收 child 尚未确认的历史；
6. 短暂进入 CUTOVER fence，停止接收新写，记录 `final_source_index`，等待 child 连续确认到该 index；
7. Master Raft 原子发布父到子路由，route epoch +1；
8. 父只返回 redirect，不再接受写；
9. 经过请求和 Snapshot 安全窗口后回收父。

不变量：

- 任一 epoch 中每个 key 只有一个可写 Slice；
- route cutover 前 child 不对 Client 写开放；
- cutover 后 parent 不再接受写；
- GLOBAL_ORDER 的 parent key range 等于两个 child key range 的无重叠并集；HASH 的 parent bucket range等于两个 child bucket range 的无重叠并集；
- 每一步以 operation ID 幂等恢复；
- catch-up exactly-once 逻辑效果由 source index 去重保证，传输层允许 at-least-once；
- parent 只有在 child cutover 完成且所有保留 Snapshot/scan 不再引用后才能释放 GC fence。

若实现初期不具备无停顿增量复制，可在 CUTOVER 使用有界短暂停写，但不能牺牲正确性。

---

## 14. 在线 Merge

仅允许同表、同 schema version、相邻 range/bucket range、配置兼容且负载较低的 Slice 合并。

```text
ACTIVE_PAIR -> PREPARING -> DUAL_COPY -> CATCHING_UP -> CUTOVER
            -> REDIRECTING -> TOMBSTONED -> GC
```

A/B 同时建立 fence，目标 C 导入两侧 Snapshot 和增量日志。两条源流分别以 `(source_slice_id, source_index)` 排序、确认和去重，并各自持有 GC fence；CUTOVER 时同时停止 A/B 新写，C 必须确认到两侧 final source index。Master 一次 Raft mutation 将 A/B 路由替换为 C。合并前后 key 覆盖集合必须完全相同。

若任一源 Slice 丢失 quorum，Merge 暂停，不能用 Merge 绕过数据不可用。

---

## 15. Replica 迁移、修复与 Balance

## 15.1 迁移流程

```text
PLAN -> ADD_LEARNER -> SNAPSHOT/CATCHUP -> PROMOTE_VOTER
     -> TRANSFER_LEADER(optional) -> REMOVE_OLD -> STABILIZE
```

禁止直接删除旧 voter 后再创建新副本。每一步写入 Master Operation Ledger，可在 Master 切主后恢复。

## 15.2 自动 Balance

Scheduler 输入：

- disk used/available；
- read/write QPS 和 bytes；
- CPU、memory、network；
- p50/p99 latency；
- leader 数、replica 数；
- Raft lag/election；
- Compaction debt、L0 文件数、flush backlog；
- 热点 RowKey/bucket sketch；
- 当前迁移和故障状态。

硬约束：

- 同 Slice 副本跨 host/rack/AZ；
- 目标节点保留容量水位；
- 单节点/单表/全集群迁移并发上限；
- 不在失去冗余时执行普通 Balance；
- 不同时迁移同一 Raft Group 多个 voter。

目标函数综合负载差、故障域、数据局部性、迁移成本和稳定性，使用 hysteresis 和 cooldown 防止来回搬迁。

## 15.3 故障修复

- heartbeat：LIVE -> SUSPECT -> DEAD；
- 使用 node UUID、boot ID、registration epoch 拒绝僵尸实例；
- quorum 存在时自动补 learner；
- quorum 丢失时停止写并告警，不做不安全强制选主；
- 周期 scrub 校验 SST checksum，损坏副本通过 Snapshot 重建；
- Master 调度优先级：恢复 quorum > 恢复副本数 > 热点处理 > 普通 Balance。

---

## 16. Master 架构

Master 自身为 3 或 5 节点 Raft Group，持久状态包括：

- Region、Table、Schema；
- Route 和 Replica placement；
- ID allocator；
- Timestamp Oracle；
- UnitServer registration；
- Split/Merge/Migrate/Repair Operation Ledger；
- mutation dedupe。

心跳时间和瞬时指标可只驻内存，但由心跳产生的调度决策必须通过 Master Raft 提交。

模块：

- SchemaManager；
- RouteManager；
- PlacementManager；
- BalanceScheduler；
- FailureDetector；
- OperationExecutor；
- TimestampOracle；
- SnapshotManager。

Master `on_apply` 是纯确定性状态转换。调度规划在 Leader 侧完成，最终 IDs、目标节点和 epoch 固化进日志。

---

## 17. API 与协议

外部服务：

### MasterService

- CreateTable / AlterTable / DropTable；
- GetTable / ListTables；
- GetRoute；
- AllocateReadTimestamp（可由 Client 自动调用）。

### DataService

- Get / BatchGet；
- Put / BatchWrite；
- Delete；
- Merge / Increment；
- CheckAndMutate；
- Seek；
- Scan。

### AdminService

- Region CRUD；
- UnitServer/List/Summary；
- Manual Split/Merge/Migrate；
- Drain UnitServer；
- Trigger Compaction/Snapshot/Scrub；
- Query Operation；
- Pause/Resume Balance。

### UnitServerService

- Register / Heartbeat；
- command acknowledgement；
- replica report；
- bootstrap/snapshot progress。

所有 mutation 都有 request ID；所有列表接口有有界分页；所有响应使用结构化错误和 RetryAction；`NOT_LEADER`、`NOT_PRIMARY`、`ROUTE_EPOCH_STALE` 携带可执行 hint。

当前 `proto/v2` 是第一版草案，后续需要根据本文补充：

- ColumnFamily 与动态 qualifier；
- MergeOperator descriptor 和 Merge RPC；
- Seek、Batch、CheckAndMutate；
- GLOBAL_ORDER 的 `next_row_key/read_ts` 与 HASH 的 per-bucket opaque scan token；
- Split/Merge/Migrate operation；
- finalized Master mutation；
- Flush/Compaction ManifestEdit；
- TSO 和 GC safe point。

---

## 18. sstv2 协同改造清单

sstv2 当前应定位为不可变 SST 文件库，不是完整 LSM。为满足 minitable，需要完成以下改造。

## 18.1 必须实现

1. **流式 Iterator API**
   - 当前 Scan 返回 `vector<Row>`，大范围扫描会全量 materialize；
   - 增加 Seek/SeekForPrev/Next/Prev；
   - key/value 分离，value 延迟读取；
   - 支持 block pin 和 iterator 生命周期错误传播。

2. **高效范围 Scan**
   - index 定位首 block；
   - block 内 lower_bound；
   - 增量解码；
   - 支持 prefix/end bound 和 reverse；
   - 不将整个结果加载到内存。

3. **Builder 流式输出**
   - 避免完整 key/value 文件驻内存后再上传；
   - 支持 sink 抽象、本地临时文件或 minidfs output stream；
   - finish 返回文件统计和 checksum。

4. **Key Codec 稳定性**
   - 明确 wire format version；
   - 提供 ordered varint/固定稳定编码；
   - 对 NaN、-0、浮点排序给出规范；
   - 增加跨平台 golden tests 和 property tests。

5. **Filter/Index 增强**
   - 支持 RowKey/Cell prefix Bloom，而不仅是完整 AllKey；
   - zone map/min-max statistics；
   - 按投影延迟解码列；
   - 为大量版本点查提供 prefix seek。

6. **Corruption 与资源保护**
   - 所有长度、offset、row count 有上限；
   - checksum 错误精确定位文件/block；
   - fuzz parser；
   - cache key 包含 file identity 和 checksum。

## 18.2 完成定义（DoD）

sstv2 改造只有在满足以下条件后才算完成：

- Iterator 与 reference vector scan 在随机 key、边界、正反向和 corruption 场景逐项对拍；
- 10 亿 key 逻辑数据范围扫描保持 O(page/block) 有界内存，不随结果总量增长；
- codec 在 GCC/Clang、x86/ARM 上通过相同 golden bytes；
- fuzz malformed SST 不越界、不 OOM、不崩溃；
- point-get、short-scan、large-scan 基准达到项目设定的 p50/p99 和吞吐门槛，回归自动阻断；
- minidfs sink 故障注入下不会产出被误判成功的文件。

## 18.3 性能增强

- block prefix/delta encoding；
- dictionary/RLE/bitpack 等 pattern；
- SIMD scalar decode 和 checksum；
- async prefetch、多 block read-ahead；
- vectored IO；
- metadata/index cache；
- separated value 批量读取；
- zero-copy key view；
- benchmark 覆盖 point get、short scan、large scan、wide row、多版本和动态 qualifier。

## 18.4 不应放入 sstv2

以下能力保留在 minitable：

- 多 SST Manifest；
- Levels 和 Compaction 调度；
- MVCC/tombstone 业务裁剪；
- MergeOperator 注册和 CF 策略；
- Raft、Split/Merge 和路由。

sstv2 可以提供通用 MergeIterator primitive，但不持有分布式或表级策略。

---

## 19. minidfs 协同改造清单

minidfs 是 SST、Manifest 和 Snapshot 的可靠对象存储层。minitable 不应假设当前未明确提供的语义。

## 19.1 必须实现或明确保证

1. **不可变文件原子发布**
   - create staging -> append -> hsync/complete -> atomic rename；
   - rename 的原子性和失败语义形成正式契约；
   - 支持 request ID 幂等；
   - close/complete 成功后所有 reader 可见完整内容。

2. **持久化屏障**
   - 区分 flush client buffer、DataNode fsync、达到期望副本数、NameNode metadata commit；
   - 提供 `hsync/complete` 明确 ACK 条件；
   - minitable 只有在 durability 条件满足后才能提交 Manifest。

3. **高效范围读取**
   - positional read；
   - multi-range/vectored read；
   - async read 和 cancellation；
   - short-circuit/local cache 可选；
   - read-ahead hint；
   - 避免每个 SST block 单独建立 RPC。

4. **一致的文件身份**
   - inode/file generation；
   - length、checksum、etag；
   - 防止 path 重用导致 cache ABA；
   - finalized immutable 文件禁止 append/overwrite。

5. **批量元数据 API**
   - batch stat/open/delete；
   - 目录分页 list；
   - 大量 SST 管理不产生 N 次串行 RPC。

6. **Snapshot/bootstrap 支持**
   - 并行下载；
   - 校验和断点续传；
   - 限速和优先级；
   - 文件 clone/hard-link/reference 能力可显著降低 Split bootstrap 成本。

7. **可靠删除与 GC**
   - 幂等 delete；
   - trash/grace period；
   - 批量删除；
   - orphan scanner；
   - 删除不能阻塞读写主路径。

8. **安全**
   - BlockToken 必须签名并校验；
   - ACL/服务身份；
   - 传输加密和可选静态加密；
   - namespace 隔离、审计和配额。

## 19.2 完成定义（DoD）

minidfs 改造只有在满足以下条件后才算完成：

- complete 成功后立即 kill Client/NameNode/DataNode，文件仍完整或明确不可见，绝不部分可见；
- rename、complete、delete 在 request ID 重试下线性一致且无重复副作用；
- 每条数据面读写路径都强制校验签名 token，缺失、过期、错误 scope 一律拒绝；
- positional/vectored read 在跨 block、短读、timeout、取消场景返回精确结果；
- metadata HA failover 不产生双 Leader 和路径回退；
- under-replication、磁盘损坏和 checksum corruption 注入后能在 SLA 内修复或明确告警；
- 批量 stat/open/delete 与并发 SST workload 达到设定 p99 和吞吐门槛。

## 19.3 HA 与可用性

若 minidfs NameNode/MySQL 仍存在单点或恢复窗口，minitable 的长期数据可用性会受其限制。必须定义并实现：

- NameNode HA 和 leader fencing；
- metadata backup/PITR；
- DataNode rack-aware replication；
- under-replication repair SLA；
- metadata 与 block report reconcile；
- minitable 在 minidfs 短时不可用时的读缓存和写入 backpressure 行为。

## 19.4 性能目标

- 大文件顺序写接近网络/磁盘带宽；
- 小范围随机读有稳定 p99；
- batch stat/open 避免 metadata bottleneck；
- 并发 Flush/Compaction/Snapshot 有租户级 QoS；
- 提供吞吐、延迟、队列、under-replication、checksum failure 指标。

---

## 20. SkipList/MemTable 协同改造清单

当前 SkipList 的单写多读模型可匹配 Raft apply，但需要：

- arena 分配，减少逐节点 new/delete；
- immutable freeze，不在线 remove；
- typed iterator 和 upper bound；
- key/value memory accounting；
- iterator pin/shared lifetime；
- apply index/commit_ts 可见性；
- 批量 prepare + atomic publish；
- benchmark 和并发 sanitizer 测试。

若 copy-on-write freeze 和 arena 足够，不必实现复杂的并发物理删除。完成定义：TSAN 下单写多读长期无竞态；freeze 前后 reference model 对拍无漏项；iterator pin 期间旧 arena 不释放；故障注入和内存压力下 prepare/publish 不产生部分可见；百万级节点 benchmark 满足内存/吞吐门槛。

---

## 21. 缓存、资源治理与热点

缓存层：

- Table/Route/Schema cache（Client）；
- Row cache（可选，必须含 read_ts/schema version）；
- Block cache（UnitServer，可按 LG/tenant 分区）；
- Index/metadata cache；
- minidfs local file/block cache。

资源控制：

- Region/table/tenant 配额；
- request/row/cell/value 最大尺寸；
- read/write QPS 和 bandwidth token bucket；
- Scan 并发、页面 bytes、CPU 解码预算；
- Flush/Compaction/Repair/Snapshot 分级 IO scheduler；
- memory pressure 时先限流、加速 Flush，禁止直接 OOM。

热点处理：

- per-Slice/RowKey prefix heavy hitter sketch；
- 自动 range split 或 hash bucket move；
- leader re-balance；
- follower read；
- row cache；
- 单 RowKey 极端热点无法通过 range split 解决，应提供明确告警和业务散列建议。

---

## 22. 可观测性与运维

必须提供：

- RPC QPS/bytes/error/p50/p99；
- Raft commit/apply latency、term、leader change、lag；
- MemTable bytes、freeze/flush latency；
- L0 文件、read amplification、compaction debt；
- cache hit、Bloom effectiveness；
- Scan rows/bytes/pages/continuation；
- Merge chain length/materialization；
- Split/Merge/Migrate 状态和耗时；
- replica/leader/load skew；
- minidfs read/write/metadata latency 和错误；
- stale route/schema reject；
- snapshot age、GC safe point、orphan files。

所有后台 operation 有可查询 ID、阶段、进度、重试次数、最后错误和人工 pause/resume/cancel（仅安全阶段允许 cancel）。

---

## 23. 安全与多租户

- service-to-service authentication；
- table/CF 级 ACL；
- Region/namespace 配额；
- TLS；
- minidfs signed token；
- 审计 DDL、管理操作和权限失败；
- 敏感值日志脱敏；
- 可选静态加密及 key rotation；
- 防止 Scan/大 value/复杂 filter 造成资源耗尽。

---

## 24. 正确性不变量

实现和测试必须覆盖：

1. 同一 Slice 任意 term 最多一个可提交写的 Leader；
2. 每个 key 在任一 route epoch 只属于一个可写 Slice；
3. schema version、route epoch、registration epoch、manifest generation 单调不回退；
4. committed Raft entry 在所有副本产生相同状态；
5. 单行 mutation 跨 LG 全可见或全不可见；
6. Snapshot 截断点不超过所有 LG flush watermark；
7. Manifest 引用的文件完整、不可变、checksum 正确；
8. 未提交 Manifest 的 SST 永不进入读路径；
9. tombstone/TTL/version GC 不删除任何活跃 snapshot 可见数据；
10. 分页 Scan 在固定 read_ts 下不漏、不重；
11. Split/Merge 前后 key 集合和版本集合保持一致；
12. request ID 重试至多产生一次逻辑 mutation；
13. Merge 在 read/compaction 不同分组下结果一致；
14. Column/CF/LG ID 永不复用；
15. 不以 UNKNOWN/UNSPECIFIED 枚举值执行默认危险操作。

---

## 25. 测试策略

### 单元测试

- Value/Schema/RowKey codec；
- tombstone 和版本可见性；
- MergeOperator algebra/property tests；
- iterator/merge iterator；
- Manifest edit/recovery；
- route lookup；
- Split/Merge 状态机每一步。

### 模型与属性测试

- 与简单 reference MVCC model 随机对拍；
- comparable 编码顺序等价性；
- compaction 前后查询结果等价；
- 任意故障点恢复后状态等价；
- Scan 分页拼接等价于一次完整 Scan。

### 故障注入

在 Raft append/commit/apply、SST upload/finalize、Manifest commit、Snapshot、route cutover、learner promote 等每个边界注入 crash、timeout、重复和乱序。

### 分布式验证

- Jepsen 风格线性一致性测试；
- 网络分区、时钟偏移、磁盘满、慢节点、checksum corruption；
- Master/UnitServer/minidfs 滚动重启；
- Split/Merge 与并发写/Scan；
- 长时间 balance 和 repair soak test。

### 性能基准

- point get、multi-get；
- small/large scan；
- narrow/wide/sparse row；
- 单/多 LG 写；
- 多版本和长 Merge chain；
- Flush/Compaction 对前台 p99 的影响；
- minidfs range read 和并发 upload。

---

## 26. 分阶段实现路线

### Phase 0：底层契约

已完成的不可变文件 I/O 子阶段：

- sstv2 提供统一 `FileSystem` 与强类型 `FileHandle` 抽象，Builder 通过 append-only handle 将 key/value 文件增量写入外部 sink，Reader 通过精确 `pread` 随机读取；
- sstv2 Reader 基于随机读打开文件，正向 Iterator 支持 `SeekToFirst` / `Seek` / `Next`，索引 `ForwardCursor` 只保留根到当前叶子的路径，内存为 O(tree height)；
- minidfs 提供 immutable-after-complete 文件、`FileIdentity(inode_id, content_generation, length, checksum)` 原子发布和身份绑定的 `read_exact`；
- minidfs DataNode 使用 positional read，只读取并校验与请求范围相交的落盘 chunk；Client 支持跨 Block 和副本回退；
- `MiniDfsFileSystem` 已通过 `create/append/close` 与 `open/read_at` 打通 sstv2 key/value 双文件的流式构建和随机读取；单文件 close/complete 原子，双文件可见性仍由后续 minitable Manifest/Raft edit 统一发布。

Phase 0 仍待完成：反向 Iterator、稳定 codec 的跨平台补强、vectored/async read、MemTable arena/freeze，以及 minitable proto v2 数据模型。

### Phase 1：单 Slice 正确闭环

- CF/LG/Cell layout；
- Put/Get/Delete/MVCC；
- Raft apply；
- Flush/Manifest/Snapshot/recovery；
- 基础 Scan + `next_row_key`。

### Phase 2：LSM 完整性

- Levels/Compaction；
- TTL/max versions/tombstone GC；
- MergeOperator；
- Batch/CheckAndMutate/Increment；
- cache 和 resource control。

### Phase 3：分布式控制面

- Master metadata Raft；
- registration/heartbeat/placement；
- route cache 和错误重试；
- learner repair/replica migration；
- TSO 和 snapshot scan。

### Phase 4：弹性

- 在线 Split/Merge；
- auto balance；
- hotspot detection；
- drain、scrub、operation ledger。

### Phase 5：生产化

- HA、安全、配额、审计；
- 故障注入和一致性验证；
- 性能调优和容量模型；
- 滚动升级和格式兼容。

每个 Phase 都必须以正确性测试、恢复测试和性能门槛作为完成条件，不能仅以“代码可编译”判定完成。
