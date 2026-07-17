# minitable 分布式表格存储架构设计

## 1. 文档定位

本文定义 minitable 的目标架构和核心语义，也是实现状态与目标契约的统一入口。除明确标注“当前已具备”的能力外，其余内容均为目标设计，不能据此推断代码已经实现。

复合 RowKey、StorageKey、MVCC 后缀的逐字节持久格式、保序证明、golden vectors 和 strict decode 规则见 [Key 编码规范](key_encoding.md)。本文只保留架构层布局；两者冲突时，已版本化的 Key 编码规范是字节格式的权威定义。

截至 2026-07-18，Phase 0 底层硬门槛已形成可测试闭环，Phase 1A 进行中：proto v2 已具备 CF、动态 qualifier、显式分区 range、128-bit Timestamp、ManifestEdit 和 SliceSnapshot 初版；KeyFormat v1 已实现 canonical StorageKey/VersionedStorageKey codec、strict decode 和首批 golden tests；arena-backed MemTable 与 `SliceStore` 已具备原子 apply/freeze/install、generation-named 持久 Manifest CAS、reopen recovery、comparator domain 强校验及 identity-fenced open/remove。LocalFS/MiniDFS 的 expected-identity remove 分别通过 unlink 前身份重验和 NameNode 元数据事务内 compare-and-delete 保证路径复用安全。Phase 1A 新增单写者串行化的 `EmbeddedSlice`：在 GLOBAL_ORDER、单 Slice、单 LG 范围内提供 Slice-local timestamp、Put/Delete、snapshot Get、正向范围 Scan、row/CF/cell tombstone、NULL/空值区分、Flush/Reopen 及时间戳高水位恢复，并已通过随机 reference MVCC model 跨 Flush 边界对拍。当前尚未完成真实 schema fingerprint 派生、proto v2 运行路径迁移、所有 crash point 注入、安全水位驱动的 Manifest/orphan GC、Raft/WAL、分页 Scan 和完整 Row size admission；未 Flush 的进程内写不具备崩溃持久性，仍不得推进 durable WAL 截断。Master 仍使用旧协议且只是服务骨架。后续修改必须同步更新本文的“当前状态”“剩余工作”和分阶段完成定义，避免目标设计与实现状态混淆。

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

### 1.1 目标工作负载与容量假设

在性能实现冻结前，必须通过 benchmark 和首个实际使用场景给出并版本化以下 workload envelope；未填写或未经验证的数值不得被称为生产门槛：

- 集群、Region、Table、Slice 和每 Slice 副本数量级；
- 目标 Slice logical/physical bytes、SST 数量和单机托管 Slice 数；
- RowKey 平均、p99 和最大编码长度；
- 每 Row 的 Cell 数、动态 qualifier 数、版本数及 Row serialized bytes 的平均、p99 和最大值；
- value 大小分布、value separation 比例和压缩率；
- point Get、short Scan、large Scan、Put、Delete 的流量比例与峰值；
- 单 Slice 和单 UnitServer 的 QPS、读写 bandwidth、CPU、内存、磁盘和网络预算；
- Flush/Compaction/Snapshot/Repair 可使用的后台资源比例；
- 故障域、复制因子及预期的节点/rack/AZ 故障模型。

所有 size、count、duration 和 concurrency 上限都必须来自该 envelope 或安全常量，并在 Schema/Region/服务配置中显式记录。超限请求返回结构化 `RESOURCE_EXHAUSTED` 或 `INVALID_ARGUMENT`，不能依赖 OOM、RPC message limit 或底层异常隐式拒绝。

### 1.2 基础服务承诺

接口成功至少表示：

- mutation 已在当前 Slice Raft 多数派 commit，并在 Leader apply 到接口承诺的可见点；
- 返回的 commit timestamp、幂等结果和 payload 已进入可恢复状态；
- Flush/Snapshot/Compaction 等后台成功不改变已经 ACK 的逻辑数据；
- strong read 的线性化点由 ReadIndex 和 applied index 共同证明；
- snapshot read 在固定 `read_ts` 和有效 snapshot lease 下不漏读、不重复、不切换视图；
- 少数派副本、进程或单节点故障不得丢失已 ACK 写；失去 quorum 时停止写而非不安全降级。

具体 latency、availability、RPO、RTO、repair time 和容量水位在生产化前由 workload envelope 和部署拓扑量化，并成为自动化验收与告警规则。本文不预设未经测量的目标数值。

### 1.3 明确非目标与分阶段限制

基础能力不包括跨 Row、跨 Slice 原子事务，也不提供跨表事务或外键。第一个可运行版本还明确不承诺：

- 跨 Slice snapshot、在线 Split/Merge、自动 Balance；
- HASH 表按原始 RowKey 全局排序；
- 反向 Scan、超大行 mid-row pagination；
- MergeOperator、Increment、CheckAndMutate 和在线 CF 跨 LG Rewrite；
- 生产级多租户隔离、跨 AZ SLA、NameNode HA 和滚动升级。

这些是后续 Phase 的目标，不得在对应正确性协议和验收条件完成前通过公开 API 暗示已支持。

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

异构列直接拼接后仍可做 byte compare 的前提、数学证明、标量变换及 STRING/BYTES 的 8+1 自终止分组格式，统一定义在 [Key 编码规范](key_encoding.md)。任何编码规则变更都必须升级 KeyFormat version。

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

每个写事务获得全局可比较的 `commit_ts`。Timestamp Oracle（TSO）的权威分配状态是 Master Raft 状态的一部分；独立、可水平扩展的 TSO gateway 从 Master 租用不重叠的 128-bit 逻辑时间 `(epoch, counter)` 区间，Slice Primary 再从 gateway 获取写时间戳。协议可压缩编码，但比较规则必须稳定。

仅保证租约区间不重叠只能提供唯一全序，不能单独提供跨 Slice snapshot：持有旧区间的 Slice 可能在 `read_ts` 分配后提交更小的 `commit_ts`，形成 late commit。因此系统同时维护 **closed timestamp**。Slice 发布 `closed_ts = T` 表示它已持久化承诺：未来不会再提交 `commit_ts <= T` 的 mutation；跨 Slice snapshot 只有在所有目标 Slice 的 `closed_ts >= read_ts` 且副本 `safe_ts >= read_ts` 时才能读取。

时间戳租约为：

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
- gateway/Slice 不得越过仍可能被旧 lease 分配的区间发布 `closed_ts`；lease 到期、显式 relinquish 或持久化耗尽后才能关闭对应区间；
- `closed_ts` 的推进决定必须进入对应 Slice Raft 日志或由等价的持久 fence 证明，重启和切主后不得回退；
- TTL 使用单独写入 Raft entry 的 `commit_physical_ms`；该值由 TSO 服务产生并受最大时钟偏移约束，apply 和历史读取不得使用本地当前时钟重新推导；
- 同一事务内所有 Cell 共享 `commit_ts` 和 `commit_physical_ms`，通过稳定 `mutation_seq` 确定同事务顺序。

Snapshot read 先向 TSO 获取 `read_ts`。每个 Replica 维护单调不回退的 `safe_ts`：只有已 apply 的日志、已安装的 Snapshot 和所有不大于该时间戳的已提交 mutation 都已可见时，才推进 `safe_ts`。Replica 服务 snapshot 必须同时满足 Slice `closed_ts >= read_ts` 与本副本 `safe_ts >= read_ts`；否则等待、转发 Primary 或返回可重试错误。这里 `closed_ts` 排除未来 late commit，`safe_ts` 证明当前副本已经追平，两者不可互相替代。

`gc_safe_point` 由活跃 scan/read lease 的最小 read_ts、备份保留点和配置保留窗口共同决定并通过 Master Raft 发布。任何低于 safe point 的新读取被拒绝；TTL、tombstone 和 `max_versions` 仅能回收 `commit_ts < gc_safe_point` 且不再被保留策略需要的数据。read lease 超时后长扫描返回 `SNAPSHOT_EXPIRED`，不得切换 read_ts。系统为 snapshot lease 设置最大存活时间；过期长读必须失败，不能无限阻塞 GC。

### 4.5.1 Phase 1 的 Slice-local Timestamp

在全局 TSO 尚未实现的单 Slice 阶段，使用独立的 timestamp domain：

```text
Timestamp {
  domain_epoch
  counter
}
```

- `domain_epoch` 在 Slice 创建时固化并进入 Snapshot；`counter` 由 Leader 在 append 前单调分配；
- 最终 `commit_ts` 和 `commit_physical_ms` 都写入 Raft entry，apply 不读取本地时钟；
- Leader 切换后从 committed/Snapshot 中的 high watermark 之后申请新 counter range，已分配值永不复用；
- Slice-local `read_ts` 只对同一 Slice 有效，不能用于跨 Slice snapshot；
- Phase 3 切换全局 TSO 时必须提交 `TimestampDomainFence`，冻结旧 domain 的最大值并启用更高的 `domain_epoch`；比较顺序先比较 domain epoch，再比较 counter；
- 同一表内不得同时接受两个未建立全序关系的 timestamp domain 写入。

该过渡协议允许 Phase 1 独立验证 MVCC，又不会把 wall clock 或裸 Raft index 固化成最终公开格式。

## 4.6 内部 Key Layout

本节是物理布局摘要；精确字节格式、canonical 规则、证明和 golden vectors 见 [Key 编码规范](key_encoding.md)。

每个 LG 使用一个 sstv2 Schema。GLOBAL_ORDER 与 HASH 使用不同的物理前缀，但共享 Cell 后缀：

```text
GLOBAL_ORDER StorageKey = LogicalRowKey
                        + RecordPrefix
                        + CfId
                        + QualifierToken

HASH StorageKey = OrderedVirtualBucketId
                + LogicalRowKey
                + RecordPrefix
                + CfId
                + QualifierToken
```

HASH 将稳定 virtual bucket ID 放在最前，使一个 bucket range 对应连续 SST key range；Split/Merge/bootstrap 可据此裁剪或复用 SST，而不必扫描整张表重新计算 bucket。逻辑 API 返回 RowKey 时不暴露该物理前缀。

KeyFormat v1 编码：

```text
RecordPrefix:
  0x00 = row tombstone marker
  0x01 = CF tombstone marker
  0x02 = ordinary cell

QualifierToken（仅 ordinary cell 存在）:
  0x00 + ordered_uint32(column_id)       = static qualifier
  0x01 + memcomparable-bytes(qualifier)  = dynamic qualifier
```

`row tombstone` 使用保留 CF ID 0，`CF tombstone` 使用目标 CF ID；两者都没有 QualifierToken 字节。用户 CF ID 从 1 开始，0 永久保留。

最终 sstv2 `AllKey`：

```text
AllKey = StorageKey
       + DescendingVersion(commit_ts, mutation_seq)
       + OpType
```

Version 必须按 `(commit_ts descending, mutation_seq descending)` memcomparable 编码，使同一 Cell 的最新版本物理相邻且优先出现。`mutation_seq` 在 canonical RowTransaction 内从 0 单调分配，row/CF/cell marker 也占用独立序号；同一请求在所有副本必须得到相同顺序。`OpType` 只用于相同逻辑版本下的确定性格式判别；正常写入必须保证同一个 `(CellKey, commit_ts, mutation_seq)` 只有一个 operation。Row/CF tombstone 必须搭配 Delete，strict encoder/decoder 拒绝其他组合。

`OrderedVirtualBucketId` 使用固定宽度 big-endian 无符号编码，宽度和 hash reduction 算法进入 Table partition format version。动态 qualifier 使用 sstv2 memcomparable bytes 编码，不允许各组件自定义 escape 规则。RowKey 中默认禁止 NaN；若未来允许，必须先固定 canonical NaN bit pattern。

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

## 5.4 CF 跨 LG Rewrite 协议

CF 移动不是一次元数据改指针，而是持久化、可恢复的在线迁移：

```text
PLANNED -> DUAL_WRITE -> BACKFILL -> VALIDATE -> CUTOVER -> CLEANUP
        -> ABORTED
```

1. Master 分配 `rewrite_id`、source/target LG、目标 schema version 和 `fence_index`；
2. Slice Raft apply `BeginCfRewrite` 后，目标 CF 的新 mutation 在同一 RowTransaction 中同时写 source/target LG，携带相同 commit_ts、mutation_seq 和 rewrite_id；
3. Backfill 在固定 `backfill_read_ts` 扫描 source，将 fence 前仍可见的完整版本历史复制到 target；输入 Manifest/ReadView 和 `gc_safe_point` 被 operation fence pin；
4. Validate 比较 key/version range checksum、行数及抽样逻辑读结果；
5. CUTOVER 通过单个 Raft schema fence 发布新 `SliceReadView`，读路径从 source 切换 target；切换前 source 权威，切换后 target 权威，不做可能返回双份 Cell 的 union read；
6. 经过 snapshot/read lease/dedupe 安全窗口后停止 dual write，并清理 source 数据；
7. abort 只允许在 CUTOVER 前执行；CUTOVER 后只能 forward-fix，不能回到旧 LG。

整个状态、进度、校验结果和 cleanup fence 进入 Snapshot/Operation Ledger。Flush、Compaction、Split 和 Snapshot 必须保留 rewrite metadata；初版可以串行化 Rewrite 与 Split/Merge，返回明确冲突而不是组合两个未验证的状态机。

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

跨 LG 读取通过一次性 pin 的不可变 `SliceReadView` 保持一致：

```text
SliceReadView {
  slice_view_generation
  visible_applied_index
  schema_version
  per_lg { active_memtable, immutable_memtable, ManifestRef }
}
```

LG 可以独立 Flush 和提交 ManifestEdit，但新 MemTable/Manifest 组合只有在构造出完整 `SliceReadView` 后一次发布。`ManifestRef` 是引用计数或 epoch-pinned 的不可变对象，不是裸 generation 数字；它持有该读视图所需 SST identity 集合。旧 MemTable、ManifestRef 和文件只有在所有引用它们的 read view、Snapshot、Compaction 和 bootstrap operation 释放后才能回收。读请求不得分别读取各 LG 的“当前指针”，否则会观察到跨 LG 部分切换。freeze 只替换完整 read view，不改变已发布节点的 `visible_applied_index`，因此对已 pin reader 透明。

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

- 同 ID、同 hash：返回首次提交的完整响应 envelope，包括 status、matched/result value、commit_ts 和必要的 route/schema hint；
- 同 ID、不同 hash：返回 `IDEMPOTENCY_CONFLICT`；
- 去重记录随 Raft apply，并进入 Snapshot；
- 去重 key 的作用域固定为 `(table_id, slice_lineage_id, client_id, request_id)`；Split/Merge bootstrap 必须迁移仍有效的记录或提供 lineage redirect 查询，不能让拓扑变化破坏幂等；
- 去重保留窗口不得短于客户端最大重试窗口；响应明确返回 dedupe expiry，过期后相同 request ID 不再承诺 exactly-once；
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
- Snapshot/Manifest 记录所需 operator 版本，缺少实现的副本不得进入 Serving；
- 任何会丢失单独 operand 版本边界的 Compaction 折叠，仅允许覆盖全部 `commit_ts < gc_safe_point` 且不被 backup/operation fence 保留的 operands；否则必须保留原链，或使用能表达完整 coverage interval 的持久格式；
- 无论正向还是反向 Scan，传给 `full_merge` 的 operands 顺序始终是 `(commit_ts ascending, mutation_seq ascending)`，结果必须一致。

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
snapshot_lease_id
lease_expiry
query_digest
served_route_epoch
```

下一页请求必须携带原 `read_ts`、有效 `snapshot_lease_id` 和相同 query digest，并以 `next_row_key` 作为 exclusive lower bound。每页请求校验或续租 lease；lease 过期统一返回 `SNAPSHOT_EXPIRED`。Client 可在每页前刷新路由，因此即使发生 Split/Merge，也能从逻辑 RowKey 继续：

- 不重复：下一页排除上一页最后一行；
- 不漏读：新路由从同一个逻辑 key 定位；
- 视图稳定：所有 Slice 在同一 read_ts 读取；
- 若 read_ts 已低于 GC safe point，返回 snapshot expired，而不是静默切换到新视图。

Phase 1 禁止 mid-row pagination：`max_row_serialized_bytes` 必须不大于 `max_scan_page_bytes`，超限写在 append 前拒绝，因此 Scan 始终在完整 Row 边界结束。后续若支持超大行，必须引入独立 `RowContinuation{row_key, last_cell_ref, last_version, read_ts, snapshot_lease_id, query_digest}`；此时 `next_row_key` 仅在当前 Row 完成后出现，不得一字段承担两种语义。

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
snapshot_lease_id
lease_expiry
route_epoch_at_start
virtual_bucket_count
hash_algorithm_version
query_digest
per_bucket_interval { start_bucket, end_bucket, next_row_key, finished }
expiry_and_signature
```

Server/Client 可并行推进多个 bucket interval；每个 bucket 内仍使用逻辑 `next_row_key` exclusive 续扫。路由变化后，Client 将 token 中的旧 bucket interval 与新 `BucketRange` 做集合切分，每个子区间继承原逻辑 cursor，再按新 Slice 路由继续；进度永不绑定旧 slice ID。连续、相同 cursor 的 bucket 可压缩成 interval，token 有严格大小上限。LG cursor 是服务端内部状态：服务端先把多个 LG 归并成完整逻辑 Row，再推进 bucket-level cursor，不向 Client 暴露 per-LG 进度。若调用方要求全局有序结果，必须选择 GLOBAL_ORDER 表，或在客户端完整收集后排序。

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

每个 LG 的 Raft-applied `manifest_generation` 是唯一真相。Manifest 对象文件不可变；本地或 minidfs 的 `CURRENT` 只是不具权威性的加速指针，可丢失、可重建。Flush/Compaction 先完成 SST 和 Manifest 对象，再提交 Raft `ManifestEdit`；只有该 entry apply 后 SST 才进入读路径。恢复时若 `CURRENT` 与 Snapshot/Raft generation 冲突，以 Snapshot+Raft 为准并重写 CURRENT，禁止形成双真相。

`ManifestEdit` 是确定性的 CAS mutation：

```text
ManifestEdit {
  edit_id
  locality_group_id
  parent_generation
  new_generation
  input_file_identities
  output_file_identities
  manifest_object_identity
  manifest_object_checksum
  flushed_applied_index
}
```

apply 规则：

1. 已存在相同 `edit_id` 时返回首次结果，不重复修改 live set；
2. `parent_generation` 必须等于该 LG 当前 generation，否则确定性记为 rejected；
3. Compaction 的 input identities 必须仍全部属于当前 live set；Flush input 由 immutable MemTable/fence identity 校验；
4. 成功时一次发布新的 `ManifestRef` 和 generation；
5. rejected edit 的 output/manifest object 只成为 orphan candidate，不得进入读路径；
6. orphan 在 grace period 后按 expected FileIdentity 幂等删除，不能仅按 path 删除。

Flush 与 Compaction 可以并发构建文件，但 ManifestEdit 在 Raft apply 顺序中串行 CAS；失败任务基于新 generation 重规划，不允许覆盖其他已提交 edit。

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
- Snapshot 保存完整 Slice lineage state，而不是单一 Manifest generation；
- 只有 `snapshot_index <= min(flushed_applied_index of all LGs)` 时才可截断日志。

Snapshot 至少包含：

```text
SliceSnapshot {
  format_version
  snapshot_index
  snapshot_term
  table_id
  slice_id
  partition_range
  schema_version
  route_epoch
  replica_set_epoch
  timestamp_domain
  timestamp_high_watermark
  visible_applied_index
  per_lg { manifest_generation, flushed_applied_index, manifest_object_identity }
  slice_mutation_dedupe
  dedupe_retention_floor
  operator_descriptors
  closed_ts
  closed_ts_fence_index
  gc_safe_point
  active_operation_fences
}
```

`safe_ts` 不作为可直接信任的独立值恢复；副本根据 Snapshot 中的 persistent fences、per-LG watermark 和 replay 后 applied state 重新计算。Snapshot 引用的每个 Manifest/SST 使用稳定 FileIdentity 并在 Snapshot 生命周期内 pin。

## 10.2 恢复

```text
load Snapshot
  -> validate format/checksum/schema/operator/timestamp-domain availability
  -> open every per-LG ManifestRef and verify referenced FileIdentity
  -> restore dedupe, closed-ts proof, GC and operation fences
  -> replay Raft entries after snapshot index
  -> rebuild Active MemTables and dedupe tail
  -> recompute visible_applied_index and safe_ts
  -> validate route/schema/replica-set fences
  -> atomically publish SliceReadView
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

文件先 MARK，再等待 generation/index/time 三重安全水位后删除。ReadView 持有的 `ManifestRef`、Snapshot、Split/Merge bootstrap 和进行中的 Flush/Compaction 都计入引用集合。删除操作携带 expected FileIdentity 并幂等；identity mismatch 只能取消本次删除并告警，不能删除当前 path 指向的新 generation。

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

virtual bucket 数创建后不变，物理 Slice 承载连续 bucket range。Split/Merge 只移动 bucket range，避免扩容时全量 rehash。HASH Slice 的物理 SST key 以 ordered bucket ID 开头，因此 bucket range 可以直接映射为连续文件范围。

HASH 表不支持按原始 RowKey 的全局有序 Scan；Scan 需并行扫描各 bucket/Slice，结果只保证每个 bucket 内 RowKey 有序。若用户需要全局 key 顺序，应使用 GLOBAL_ORDER。

## 11.3 Route Epoch

每次 Split、Merge、迁移切主或 replica route 变化使 `route_epoch` 单调增加。Data 请求携带 table ID、slice ID、schema version、route epoch；读写响应携带实际 `served_route_epoch`、`served_replica_set_epoch`、Slice term 和 applied index。旧请求得到结构化 hint，不能被错误路由到其他数据。

路由协议必须使用显式分区范围，而不是让 `start_key/end_key` 同时承担两种语义：

```text
SliceRoute {
  slice_id
  oneof partition_range {
    KeyRange { encoded_start_key, encoded_end_key }
    BucketRange { start_bucket, end_bucket }
  }
  replicas
}
```

两种范围均采用半开区间 `[start, end)`；GLOBAL_ORDER 空 key 边界表示无穷，HASH bucket 边界必须显式给出。

---

## 12. 一致性与 Raft

每个 Slice 是独立 Raft Group：

- Primary 对应 Raft Leader；
- 写在多数派 commit 后成功；
- strong read 使用 ReadIndex，确认 Leader 权限和 applied index；
- lease read 仅作为可证明安全的优化；
- bounded-staleness read 要求 follower safe time/lag 满足限制；
- eventual read 可直接读 follower 已应用状态，但仍必须通过 route、replica-set 和本地 Serving fence。

Follower 只有在以下条件全部满足时才能服务：本地 Slice identity 与请求一致；已应用的 `route_epoch` 和 `replica_set_epoch` 不低于请求；自身仍在当前 replica set；请求 read mode 的 `safe_ts/applied_index` 条件成立；未处于 installing snapshot、tombstoned 或 corruption 状态。Follower 无法仅凭本地旧状态证明 membership 时必须拒绝或向 Leader/Master 验证，不能以 eventual 语义绕过 fencing。

配置变更使用 joint consensus。新副本先作为 learner 追赶，达到 lag 阈值后提升 voter。`replica_set_epoch` 随 committed config change 单调推进并进入 Snapshot；被移除副本在 apply removal fence 后关闭 Serving，再等待 read lease 和本地引用释放后清理数据。

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
5. child 建立 Raft Group、安装 Snapshot、从 fence index + 1 追平 mutation；bootstrap snapshot 除 Cell/SST 外还携带 schema/operator、timestamp domain、per-LG Manifest、closed-ts proof、仍有效 dedupe 记录、CF rewrite 状态和 GC/backup fences；追赶期间父 Slice 为该 operation 注册 `cutover_gc_fence`，禁止 Compaction/TTL/版本 GC 回收 child 尚未确认的历史；
6. 短暂进入 CUTOVER fence，停止接收新写，记录 `final_source_index`，等待 child 连续确认到该 index；
7. Master Raft 原子发布父到子路由，route epoch +1；
8. 父只返回 redirect，不再接受写；
9. parent 为 route cutover 前创建的 snapshot lease 继续提供只读 lineage 服务，或将 lease/read_ts 与逻辑 cursor 映射到 children；只有所有旧 lease、dedupe redirect 和 Snapshot 安全窗口结束后才回收父。

不变量：

- 任一 epoch 中每个 key 只有一个可写 Slice；
- route cutover 前 child 不对 Client 写开放；
- cutover 后 parent 不再接受写；
- GLOBAL_ORDER 的 parent key range 等于两个 child key range 的无重叠并集；HASH 的 parent bucket range 等于两个 child bucket range 的无重叠并集；
- 每一步以 operation ID 幂等恢复；
- catch-up exactly-once 逻辑效果由 source index 去重保证，传输层允许 at-least-once；
- parent 只有在 child cutover 完成且所有保留 Snapshot/scan 不再引用后才能释放 GC fence；
- Split 不得重写 commit_ts、mutation_seq、operator envelope 或未过期 dedupe response；
- child 从 inherited timestamp high watermark 之后分配时间戳，且不得发布低于 inherited closed_ts 的状态。

若实现初期不具备无停顿增量复制，可在 CUTOVER 使用有界短暂停写，但不能牺牲正确性。

---

## 14. 在线 Merge

仅允许同表、同 schema version、相邻 range/bucket range、配置兼容且负载较低的 Slice 合并。

```text
ACTIVE_PAIR -> PREPARING -> DUAL_COPY -> CATCHING_UP -> CUTOVER
            -> REDIRECTING -> TOMBSTONED -> GC
```

A/B 同时建立 fence，目标 C 导入两侧 Snapshot 和增量日志。两条源流分别以 `(source_slice_id, source_index)` 排序、确认和去重，并各自持有 GC fence；Snapshot 同时导入 schema/operator、timestamp high watermark、closed-ts proof、仍有效 dedupe response、CF rewrite 和 GC/backup fences。C 以两个 source domain high watermark 之上的新 timestamp domain 继续写，不能复用任一未耗尽 lease。CUTOVER 时同时停止 A/B 新写，C 必须确认到两侧 final source index。Master 一次 Raft mutation 将 A/B 路由替换为 C。合并前后 key/version 集合、未过期幂等语义和活跃 snapshot lease 可见结果必须完全相同；旧 lease 可由 A/B lineage read service 服务到期，或原子映射到 C。

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

当前 `proto/v2` 是第一版草案，已表达 ColumnFamily、动态 qualifier、显式 Key/Bucket range、128-bit Timestamp、canonical RowTransaction、ManifestEdit 和稳定 CellRef，但尚未接入 Master/DataService，也未完成兼容性冻结。后续需要根据本文补充：

- MergeOperator descriptor 和 Merge RPC；
- Seek、Batch、CheckAndMutate；
- GLOBAL_ORDER 的 `next_row_key/read_ts` 与 HASH 的 per-bucket opaque scan token；
- Split/Merge/Migrate operation；
- 完整 TSO、timestamp domain fence、GC safe point 与 snapshot lease；
- `SliceSnapshot` 的 dedupe retention、operator descriptor 和 active operation fence；
- proto reserved-field、wire compatibility 和语义校验测试。

---

## 18. sstv2 协同改造清单

sstv2 当前应定位为不可变 SST 文件库，不是完整 LSM。为满足 minitable，需要完成以下改造。

## 18.1 当前已具备（截至 2026-07-17）

以下能力已有实现和单元测试，`bazel test //cpp/pl/sstv2/...` 当前 16/16 通过：

- 统一 `FileSystem`、强类型 `FileHandle`、`LocalFileSystem` 和 `MiniDfsFileSystem`；
- append-only 流式 Builder，按 block 行数/大小有界缓冲，增量写 key/value 文件并构建多层索引；`FinishResult` 返回 key/value FileIdentity、row count 和 min/max encoded key；
- 正向流式 Iterator：`SeekToFirst`、`Seek`、`Next`、start/limit bound，`ForwardCursor` 状态为 O(tree height)；
- 正向 k-way `MergeIterator` primitive，支持 lower-bound seek、稳定 source priority、重复 physical key 和 sticky child error；
- memcomparable 基础 codec：整数、浮点、STRING/BYTES、ASC/DESC、复合 RowKey、128-bit `Version{major, minor}` 和 OpType；bytes decoder 拒绝非 canonical padding/终止组；
- canonical shortest big-endian `ordered_uint32`；
- embedded/separated value、Snappy/Zstd、完整 AllKey Bloom；
- block、Bloom、Tail 和 separated value CRC32C，short read、格式版本及多类 corruption 检测；
- MiniDFS sink 上的 SST 构建、point Get、正向 Iterator 和 Seek 已有跨组件 E2E 场景。

旧 `Reader::scan()` 仍返回 `vector<Row>`，但 minitable 必须使用现有流式 Iterator，不得以旧接口实现大范围 Scan。

## 18.2 剩余必须实现

进入单 Slice LSM 闭环前必须完成：

1. **minitable Key 契约（部分完成）**
   - 已实现 GLOBAL_ORDER/HASH `StorageKey`、QualifierToken、降序 Version、NaN 拒绝、signed-zero canonicalization、strict decode 和首批 golden vectors；
   - 仍需将 `key_format_version + row_key_schema_fingerprint` 固化到 Table metadata、Manifest 和 SST properties，并由 Reader 校验 comparator domain；
   - 仍需补齐随机 property/fuzz、完整边界矩阵及 GCC/Clang、x86/ARM golden compatibility。

2. **Manifest 构建结果（部分完成）**
   - Builder `FinishResult` 已返回 min/max encoded key、entry count 和 key/value finalized FileIdentity；
   - 仍需返回并持久化 SST format version、KeyFormat/comparator domain、schema fingerprint 和 checksum algorithm；
   - `open/remove` 已支持 expected identity；MiniDFS expected-remove 是事务内原子 compare-and-delete，LocalFS 在受管私有目录假设下执行 inode/length/checksum 校验与 unlink 前重验；后续 Manifest/GC 必须使用该接口，禁止只依赖 path。

3. **多路归并 primitive（primitive 已完成）**
   - 正向 k-way `MergeIterator` 已可归并任意 `ForwardCursor`；仍需提供真正的 SST Reader cursor adapter 并接入 MemTable + SST read view；
   - sstv2 只负责机械有序归并，MVCC、tombstone、TTL 和 MergeOperator 仍由 minitable 处理；source priority 仅作为相同 key 的稳定机械 tie-breaker。

在承诺完整 Data API 前必须完成：

4. **反向 Iterator**：`SeekToLast`、`SeekForPrev`、`Prev` 和 reverse bound；
5. **延迟 value 与缓存生命周期**：key/value 分离读取、Block Cache、FileIdentity cache key、block pin；
6. **Filter/Index 增强**：RowKey/Cell prefix Bloom、zone map/min-max statistics；
7. **Corruption 与资源保护**：所有长度/offset/count 上限、fuzz parser、资源预算和错误定位。

## 18.3 完成定义（DoD）

sstv2 改造只有在满足以下条件后才算完成：

- Iterator 与 reference vector scan 在随机 key、边界、正反向和 corruption 场景逐项对拍；
- 10 亿 key 逻辑数据范围扫描保持 O(page/block) 有界内存，不随结果总量增长；
- codec 在 GCC/Clang、x86/ARM 上通过相同 golden bytes；
- fuzz malformed SST 不越界、不 OOM、不崩溃；
- point-get、short-scan、large-scan 基准达到项目设定的 p50/p99 和吞吐门槛，回归自动阻断；
- minidfs sink 故障注入下不会产出被误判成功的文件。

## 18.4 性能增强

- block prefix/delta encoding；
- dictionary/RLE/bitpack 等 pattern；
- SIMD scalar decode 和 checksum；
- async prefetch、多 block read-ahead；
- vectored IO；
- metadata/index cache；
- separated value 批量读取；
- zero-copy key view；
- benchmark 覆盖 point get、short scan、large scan、wide row、多版本和动态 qualifier。

## 18.5 不应放入 sstv2

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

## 19.1 当前已具备（截至 2026-07-17）

以下能力已有实现和单元测试，MiniDFS 核心测试当前 21/21 通过：

- staging/create、流式 append、DataNode `fsync/fdatasync`、block commit 和 NameNode complete；
- immutable-after-complete，完成后禁止 append/truncate，并原子发布 `FileIdentity(inode_id, content_generation, length, checksum)`；
- identity-bound `read_exact`，支持 positional read、跨 Block、short-read 拒绝、chunk/整文件 checksum 和 replica fallback；
- request ID/oplog 幂等覆盖主要 metadata mutation；
- HMAC-SHA256 BlockToken、权限/过期/identity 绑定及 DataNode 强制校验；
- 删除标记、heartbeat delete command、DataNode trash 和 `purge_trash()` 基础链路；
- `MiniDfsFileSystem` 已打通 sstv2 key/value 双文件的流式构建与随机读。

当前 complete 的 block durability 至少要求 `min(kMinWriteReplica, desired_replica)` 个 finalized replica；minitable 只有在所有 SST 文件 complete 并取得稳定 identity 后才能提交 ManifestEdit。

## 19.2 剩余必须实现或明确保证

进入单 Slice Manifest 闭环前必须完成：

1. **跨层文件身份契约（部分完成）**
   - sstv2 `close/complete` 已向上返回 finalized FileIdentity，`FinishResult` 已携带 key/value identity；
   - `open/remove` 仍需支持 expected identity；GC 不得仅凭可复用 path 删除文件；
   - Manifest 尚需完整保存 key/value identity、format/domain 信息；key/value 双文件不要求 minidfs 跨文件事务，由 minitable Raft ManifestEdit 统一发布。

2. **故障边界测试**
   - 覆盖 key/value 分别 complete、响应丢失、UnitServer crash、Manifest 未提交 orphan、identity mismatch；
   - referenced SST 缺失或 checksum 错误时副本不得 Serving。

稳定运行和生产化前必须完成：

3. **Lease recovery 与 orphan GC**：过期未 complete 的 minitable staging 文件默认回收而非伪造 checksum 发布；补齐周期 trash purge 和 deleted metadata 清理；
4. **高效范围读取**：multi-range/vectored/async read、cancellation 和 read-ahead；
5. **批量元数据**：batch stat/open/delete 和分页 list；
6. **Snapshot/bootstrap**：并行下载、校验、断点续传、限速；clone/reference 为可选优化；
7. **HA 与资源治理**：NameNode HA/leader fencing、rack-aware placement、QoS/backpressure、监控和修复 SLA；
8. **安全**：ACL/服务身份、TLS、namespace 隔离、审计和配额。

## 19.3 完成定义（DoD）

minidfs 改造只有在满足以下条件后才算完成：

- complete 成功后立即 kill Client/NameNode/DataNode，文件仍完整或明确不可见，绝不部分可见；
- rename、complete、delete 在 request ID 重试下线性一致且无重复副作用；
- 每条数据面读写路径都强制校验签名 token，缺失、过期、错误 scope 一律拒绝；
- positional/vectored read 在跨 block、短读、timeout、取消场景返回精确结果；
- metadata HA failover 不产生双 Leader 和路径回退；
- under-replication、磁盘损坏和 checksum corruption 注入后能在 SLA 内修复或明确告警；
- 批量 stat/open/delete 与并发 SST workload 达到设定 p99 和吞吐门槛。

## 19.4 HA 与可用性

若 minidfs NameNode/MySQL 仍存在单点或恢复窗口，minitable 的长期数据可用性会受其限制。必须定义并实现：

- NameNode HA 和 leader fencing；
- metadata backup/PITR；
- DataNode rack-aware replication；
- under-replication repair SLA；
- metadata 与 block report reconcile；
- minitable 在 minidfs 短时不可用时的读缓存和写入 backpressure 行为。

## 19.5 性能目标

- 大文件顺序写接近网络/磁盘带宽；
- 小范围随机读有稳定 p99；
- batch stat/open 避免 metadata bottleneck；
- 并发 Flush/Compaction/Snapshot 有租户级 QoS；
- 提供吞吐、延迟、队列、under-replication、checksum failure 指标。

---

## 20. SkipList/MemTable 协同改造清单

当前 MemTable 已实现 arena-backed 多版本 key/value、两阶段 batch、immutable freeze、固定水位正向 lower-bound cursor、shared ownership lifetime、严格递增 apply index 和逻辑写预算；其有序索引仍使用 `std::map + shared_mutex`，不是最终生产 SkipList。`SliceStore` 通过单写 `apply_mutex_` 和不可变 `PublishedReadState` 原子发布 Active/Immutable/SST 组合，已实现 token-fenced Flush/install/retire、持久 generation-named Manifest 和 Reopen；当前 live set 尚未由 Raft 管理，旧 Manifest 与 crash orphan 的安全水位 GC 也未完成。

进入生产级 MemTable 与多 LG 前仍需：

- 批量 prepare + atomic publish，以及 read-visible apply index/commit_ts watermark；
- 分配失败可 rollback/reservation，避免 arena 已消耗但写入未发布；
- 明确重复完整 VersionedStorageKey 的幂等校验，禁止静默覆盖冲突值；
- 包含 map node、arena block rounding 和 allocator metadata 的一致内存记账/admission；
- typed range upper bound、benchmark 和并发 sanitizer 测试；
- 多 LG 阶段的跨 LG atomic visibility。

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
15. 不以 UNKNOWN/UNSPECIFIED 枚举值执行默认危险操作；
16. timestamp domain 切换和 Leader 切换后 commit_ts 永不复用、全序不回退；
17. ManifestEdit 只有 parent generation 和 input identities 同时匹配才可发布；
18. 被移除、过期 route 或过期 replica-set 的副本不得服务任何一致性级别的读；
19. Split/Merge/CF Rewrite 不丢失未过期 dedupe response、snapshot lease 和 GC fence；
20. 任意 ReadView 引用的 MemTable、Manifest 和 SST 在 view 释放前不得回收。

---

## 25. 测试策略

### 单元测试

- Value/Schema/RowKey codec；
- tombstone 和版本可见性；
- MergeOperator algebra/property tests；
- iterator/merge iterator；
- Manifest CAS 冲突、orphan recovery 和 FileIdentity fencing；
- route lookup；
- Split/Merge/CF Rewrite 状态机每一步；
- Slice-local/global timestamp domain fence、closed_ts 和 safe_ts；
- dedupe response 的冲突、过期与 lineage 迁移。

### 模型与属性测试

- 与简单 reference MVCC model 随机对拍；
- comparable 编码顺序等价性；
- compaction 前后查询结果等价；
- 任意故障点恢复后状态等价；
- Scan 分页拼接等价于一次完整 Scan；
- HASH token 经任意合法 bucket-range 切分后进度等价；
- Merge operand 经任意合法 Compaction 分组后结果等价。

### 故障注入

在 Raft append/commit/apply、SST upload/finalize、Manifest CAS、Snapshot save/install、timestamp domain fence、CF Rewrite、route cutover、learner promote 和文件 GC 等每个边界注入 crash、timeout、重复和乱序。

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

当前状态：**Phase 0 底层硬门槛已形成可测试闭环；跨平台兼容性、proto 迁移和安全 GC 作为进入 Phase 1B 前的加固项继续推进。**

已完成或已有可测试初版：

- sstv2 提供统一 `FileSystem` 与强类型 `FileHandle` 抽象，Builder 通过 append-only handle 将 key/value 文件增量写入外部 sink，Reader 通过精确 `read_at` 随机读取；
- sstv2 Reader 基于随机读打开文件，正向 Iterator 支持 `SeekToFirst` / `Seek` / `Next` 和 start/limit bound，索引 `ForwardCursor` 只保留根到当前叶子的路径，内存为 O(tree height)；
- minidfs 提供 immutable-after-complete 文件、`FileIdentity(inode_id, content_generation, length, checksum)` 原子发布和身份绑定的 `read_exact`；`MiniDfsFileSystem` 已打通 SST key/value 双文件构建、随机读取和跨组件 E2E；
- proto v2 已表达 CF、动态 qualifier、显式分区 range、128-bit Timestamp、ManifestEdit、SliceSnapshot 初版和稳定 CellRef；
- KeyFormat v1 已实现 GLOBAL_ORDER/HASH StorageKey、QualifierToken、降序 Version、NaN/signed-zero 规则、strict decode 和首批 golden tests；
- sstv2 Builder `FinishResult` 已返回 key/value finalized FileIdentity、row count 和 min/max key；
- 正向 k-way `MergeIterator` primitive 已实现；
- MemTable 已实现带 owner/generation 校验的 arena checkpoint/rewind、受逻辑写预算约束的 batch reservation、可失败 prepare 与无失败 publish、严格递增 apply-index 版本链、固定 read-visible watermark 的正向 cursor、freeze 和 shared lifetime；`SliceStore` 独占 writable MemTable，以不可变 `SliceVersion` 和单个 `PublishedReadState` 原子绑定结构 generation 与数据可见水位，已实现跨 LG 两阶段 apply、单 Immutable slot 的 Active→Immutable freeze、opaque Flush token、per-Immutable identity/fence、锁外 sstv2 Builder、FileIdentity-fenced SST open、SST install 与 Immutable retire 的单次原子发布、旧 read view pin，以及 Active→Immutable→新到旧 SST 的去重合并 cursor。generation-named 持久 Manifest、deterministic CAS、bounded install ledger 与 Reopen 已落地，LocalFS E2E 覆盖 Flush 前后读等价、crash-before-install、reopen 后继续安装、多代 SST、错误 provenance、构建失败、Manifest/SST 篡改拒绝；关键并发/identity 测试已重复运行。

Phase 0 进入 Phase 1 前仍待完成的硬门槛：

- 将当前占位的 row-key schema fingerprint 替换为由真实 Table RowKey Schema 派生并在 Table metadata、Manifest、SST properties 三处一致强校验的值，补齐随机 property/fuzz 与跨平台 golden tests；
- 完成 proto v2 compatibility/reserved-field 审计，并将运行路径从旧 proto 迁移到 v2；
- 基于 pinned Manifest/read view 安全水位实现旧 Manifest 与 finalized/broken orphan SST 的异步 GC；
- 将当前 `SliceStore` 接入后续 Slice Raft apply/runtime，继续验证 committed apply 的 publish 路径保持无失败；在 Raft Snapshot 完成前不得推进 durable flush watermark 或截断 WAL。

反向 Iterator、lazy value、Block Cache、prefix Bloom、vectored/async read 不阻塞正向单 Slice 闭环，但在对外承诺对应功能或性能目标前必须完成。

### Phase 1A：嵌入式单 Slice MVCC

目标：不依赖 Master、网络 Raft 和 minidfs 集群，先证明核心 Cell 语义。

当前状态：**进行中。** `EmbeddedSlice` 已在 GLOBAL_ORDER、单 Slice、单 LG 范围实现串行 timestamp/write/freeze/flush，读路径通过 pinned `SliceReadView` 并发执行；已具备 Put/Delete、snapshot Get、`[start,end)` 正向 Scan、row/CF/cell tombstone、同 timestamp 的 mutation_seq 屏障顺序、NULL/空值区分、Flush/Manifest/Reopen 和高水位恢复。确定性随机用例已与 reference model 在 Active、Immutable、SST 混合视图及 Flush 边界对拍，并覆盖并发 writer timestamp 唯一性、精确 Get 不越行、范围边界和 tombstone 后重建。

1. 冻结 codec、CellRef、tombstone、Version 和 Slice-local timestamp domain；
2. 单 Slice、单 LG：Put/Get/Delete、MVCC、正向 Seek/Scan；
3. MemTable arena/freeze、k-way MergeIterator 和完整 Row 聚合；
4. 基于临时目录或 in-process FileSystem 完成 Flush、Manifest CAS 和 reopen recovery；
5. 对 reference MVCC model 做随机对拍，并在 Flush/Manifest 各边界注入 crash。

剩余工作：补齐真实 schema fingerprint、完整 Cell value canonical/schema validation、Row 聚合尺寸 admission 与无 mid-row pagination 约束；为 SST build/finalize/Manifest publish 的每个故障点增加可枚举 fault injection，并实现 broken/finalized orphan 清理；将 timestamp high watermark 直接固化到 Manifest，避免 Reopen 全表扫描。

退出条件：codec golden/property test 稳定；未提交文件不可见；任意 crash 后逻辑查询结果与 reference model 等价；无 mid-row pagination。

### Phase 1B：单 Slice Raft 与 minidfs 持久闭环

1. Slice Raft deterministic apply、完整 dedupe response、commit physical time；
2. minidfs streaming Flush、FileIdentity 校验与 Raft `ManifestEdit` CAS；
3. 完整 `SliceSnapshot`、日志截断条件和 Snapshot 安装；
4. append/commit/apply、SST complete、Manifest commit、Snapshot save/install 全故障点恢复；
5. 扩展到多 LG，引入 pin 的 `SliceReadView` 和跨 LG prepare/publish；
6. GLOBAL_ORDER 分页 Scan 使用固定 read_ts、snapshot lease 和 `next_row_key`。

退出条件：单 Slice 三副本切主、重启和 Snapshot repair 后与 reference model 等价；已 ACK 写不丢失；所有 orphan 可安全回收；跨 LG 不出现部分可见。

Phase 1 不要求跨 Slice snapshot、在线 Split/Merge、反向 Scan、完整 Compaction 或生产级 NameNode HA。

### Phase 2A：基础 LSM 完整性

- L0/L1+ Version、Compaction CAS、并发 Flush/Compaction 冲突重规划；
- TTL/max versions/tombstone GC 与 snapshot lease/gc safe point；
- Block Cache、admission control、前后台资源调度；
- 文件 GC、scrub、checksum corruption repair。

退出条件：Compaction 前后任意 read_ts 查询等价；活跃 lease 下无版本误删；磁盘满、慢 IO、checksum error 均无假成功和无界资源增长。

### Phase 2B：高级单行语义

- MergeOperator 与 coverage-aware Compaction；
- Batch、CheckAndMutate、Increment；
- CF 跨 LG Rewrite；
- 反向 Iterator/Scan 和可选的超大行 continuation。

退出条件：Merge algebra/property test、请求重试、切主、Rewrite crash/recovery 和正反向 Scan 结果一致；不支持的高级功能继续显式拒绝。

### Phase 3A：Master 与多 Slice 基础

- Master metadata Raft、真实的 metadata command apply 和 Snapshot；
- registration/heartbeat/placement、route cache、route/replica fencing；
- learner repair、replica migration 和 operation ledger；
- 多 Slice point read/write 和非 snapshot Scan。

退出条件：Master 切主后 operation 可恢复；stale route/stale replica 一律被 fence；节点增删与 learner repair 不丢失 ACK 数据。

### Phase 3B：全局时间与跨 Slice Snapshot

- TSO gateway、timestamp lease 和 Slice-local domain 到全局 domain 的迁移 fence；
- closed timestamp、safe_ts、snapshot lease 和 gc safe point；
- GLOBAL_ORDER 跨 Slice 分页与 HASH opaque token/remap；
- 跨 Slice Scan 在 route refresh 和副本切换下固定 read_ts。

退出条件：证明并测试无 late commit；长 Scan 在并发写、切主和路由刷新下不漏不重；lease expiry 可回收历史且统一返回 `SNAPSHOT_EXPIRED`。

### Phase 4：弹性

- 在线 Split 及长读/dedupe/timestamp/operation state 继承；
- 在线 Merge 及双 source lineage 合并；
- auto balance、hotspot detection、drain 和 scrub；
- Split/Merge 与 Rewrite 初期互斥，分别稳定后才允许组合。

退出条件：每个状态迁移 crash 可恢复；cutover 前后 key/version/dedupe/snapshot 语义等价；故障注入与长时间 soak test 通过。

### Phase 5：生产化

- 依据 workload envelope 量化 SLO、容量模型和 overload policy；
- Master/minidfs/UnitServer HA、故障域放置和灾难恢复演练；
- 安全、配额、审计、多租户隔离；
- 格式兼容矩阵、滚动升级、降级和回滚；
- Jepsen 风格一致性验证、性能回归和 release qualification。

退出条件：明确部署拓扑下的 availability/RPO/RTO、容量和性能门槛均由自动化测试或演练证明。

每个 Phase 必须同时具备功能契约、正确性不变量、恢复测试、资源上限和可复现性能基线；不能仅以“代码可编译”或 happy-path E2E 判定完成。Phase 的公开 API 必须与该阶段已证明能力一致，未完成能力默认关闭。
