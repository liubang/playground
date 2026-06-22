# minitable — 架构设计文档

## 1. 概述

minitable 是一个简化版分布式表格存储系统，仅依赖以下内部组件：

| 依赖              | 用途                                                              |
| ----------------- | ----------------------------------------------------------------- |
| `minidfs`         | 分布式文件系统 — SSTable 文件 + 元信息持久化 (唯一外部存储依赖)   |
| `sstv2`           | LSM 磁盘引擎 (类型化 Schema、多级索引、Bloom、memcomparable 编码) |
| `braft`           | Raft 共识 — WAL 复制、Leader 选举、Snapshot/恢复                  |
| `brpc`            | RPC 框架                                                          |
| `cpp/pl/skiplist` | MemStore 内存有序写缓冲                                           |

## 2. 核心概念

| 概念              | 定义                                                                      |
| ----------------- | ------------------------------------------------------------------------- |
| **Cluster**       | 一个 minitable 集群，包含 1 组 Master + 若干 Region                       |
| **Master**        | 3 实例 (braft HA)，管理集群元信息、DDL、Region 划分、路由表               |
| **Region**        | 逻辑资源池（多租户/业务隔离），一个 Region 下有 N 个 UnitServer           |
| **Table**         | 用户表，有一个可选的 namespace 字段做逻辑隔离，被水平切分为多个 Slice     |
| **LocalityGroup** | 表的列子集，每个 LG 在 Slice 内有独立的 LSM 引擎实例 (MemStore + SSTable) |
| **UnitServer**    | 承载 Slice 的工作节点，管理本机上的 Slice 实例 (braft Node)               |
| **Slice**         | 一张表的一个 key-range 分片，N 个副本 (通常 3)，副本间通过 braft 同步     |
| **Replica**       | Slice 的一个副本，角色为 Leader 或 Follower                               |

## 3. 集群拓扑

```
Cluster
 │
 ├── Master (3 实例, braft HA)
 │    ├── Schema Manager     — DDL (CREATE/ALTER/DROP TABLE)
 │    ├── Region Manager     — Region CRUD, UnitServer 管理
 │    ├── Slice Scheduler    — Slice 分配, 负载均衡
 │    └── Route Table        — (ns, table) → [Slice key-ranges + Replica 地址]
 │
 ├── Region "prod"
 │    ├── UnitServer-0
 │    │    ├── Slice [ns:"app_db", table:"user_profile", 0]
 │    │    │    ├── Replica 0 (Leader)   ← 主副本, 负责写
 │    │    │    ├── Replica 1 (Follower) ← 从副本, 可配读
 │    │    │    ├── Replica 2 (Follower)
 │    │    │    └── LG "default" (MemStore + SSTables)
 │    │    │        LG "meta"   (独立的 MemStore + SSTables)
 │    │    └── Slice [...]
 │    ├── UnitServer-1
 │    └── UnitServer-N
 │
 └── Region "test"
      └── ...
```

## 4. 数据模型

minitable 采用 **宽列 (Wide Column)** 模型，在 sstv2 的类型化 Schema 之上增加半结构化能力。

### 4.1 DDL

```sql
CREATE TABLE user_profile (
    NAMESPACE 'app_db',        -- namespace 字符串, 逻辑隔离, 默认 ""
    tenant      STRING,
    user_id     UINT64,
    ───────────                -- 以上为 RowKey

    LG default (               -- LocalityGroup "default"
        cf1:name    STRING,
        cf1:email   STRING?,
        cf1:age     UINT32?,
    ),

    LG meta (                   -- LocalityGroup "meta", 独立 LSM 引擎
        cf2:tags    ARRAY[STRING],
        cf2:meta    MAP[STRING, STRING]?,
    )
)
PARTITION BY HASH(tenant, user_id) SLICES=16
REPLICAS=3
REGION='prod';
```

### 4.2 Schema 映射

每个 LG 生成独立的 sstv2::Schema，共享相同的 RowKey 列：

```cpp
// LG "default" 的 Schema
auto default_schema = sstv2::SchemaBuilder()
    .add_sort_key("tenant",   DataType::kString, SortOrder::kAscending)
    .add_sort_key("user_id",  DataType::kUint64, SortOrder::kAscending)
    .add_column("cf1:name",   DataType::kString)
    .add_column("cf1:email",  DataType::kString)   // optional
    .add_column("cf1:age",    DataType::kUint32)   // optional
    .build();

// LG "meta" 的 Schema — 独立引擎, 可不同压缩策略
auto meta_schema = sstv2::SchemaBuilder()
    .add_sort_key("tenant",   DataType::kString, SortOrder::kAscending)
    .add_sort_key("user_id",  DataType::kUint64, SortOrder::kAscending)
    .add_column("cf2:tags",   DataType::kArray)
    .add_column("cf2:meta",   DataType::kMap)      // optional
    .build();
```

sstv2 的 InternalSchema 自动为每一行添加系统列：`version` (多版本), `op_type` (PUT/DELETE), `flag` (tombstone 标记), `filename`, `offset`, `length`, `checksum`。

### 4.3 半结构化存储

**Optional Column** — 行之间可以有不同的列集合，缺少的列存 NULL：

```cpp
Row::make(schema, {{"tenant", "acme"}, {"user_id", 42},
                    {"cf1:name", "Alice"}, {"cf1:email", "alice@acme.com"}});
Row::make(schema, {{"tenant", "acme"}, {"user_id", 43},
                    {"cf1:name", "Bob"}});  // cf1:email omitted
```

**Map Column** — 动态字段，不同行可有完全不同的 key：

```cpp
Row::make(schema, {{"cf2:meta", Map{{"department", "eng"}, {"title", "staff"}}}});
Row::make(schema, {{"cf2:meta", Map{{"location", "beijing"}, {"manager", "alice"}}}});
```

**Array Column** — 动态长度：

```cpp
Row::make(schema, {{"cf2:tags", Array{"devops", "sre"}}});
Row::make(schema, {{"cf2:tags", Array{"backend", "golang", "cpp", "rust", "ops"}}});
```

### 4.4 数据操作

**Put** — 单行多列原子写入：

```cpp
client.put("app_db", "user_profile",
           RowKey{{"tenant", "acme"}, {"user_id", 42}},
           {{"cf1:name", "Alice"}, {"cf1:email", "alice@acme.com"},
            {"cf2:tags", Array{"devops"}}});
```

语义：同 (rowkey, column) 已存在则追加新版本，不存在则插入。timestamp 自动分配 (微秒)。

**Get** — 支持指定列、指定版本数：

```cpp
auto row = client.get("app_db", "user_profile",
                      RowKey{{"tenant", "acme"}, {"user_id", 42}},
                      {"cf1:name", "cf1:email"});
```

**Scan** — 范围扫描，支持过滤：

```cpp
auto iter = client.scan("app_db", "user_profile",
                        RowKey{{"tenant", "acme"}, {"user_id", 0}},
                        RowKey{{"tenant", "acme"}, {"user_id", 100}},
                        ScanOptions{.columns = {"cf1:name"},
                                    .filter = Filter::column("cf1:age").gt(18)});
```

**Delete** — 追加 tombstone，Compaction 时物理删除：

```cpp
client.delete_column("app_db", "user_profile",
                     RowKey{{"tenant", "acme"}, {"user_id", 42}}, "cf1:email");
client.delete_row("app_db", "user_profile",
                  RowKey{{"tenant", "acme"}, {"user_id", 42}});
```

### 4.5 分区

| 类型             | 语法                                           | 说明                                                  |
| ---------------- | ---------------------------------------------- | ----------------------------------------------------- |
| **HASH**         | `PARTITION BY HASH(tenant, user_id) SLICES=16` | hash % N 决定 Slice，均匀分布                         |
| **GLOBAL_ORDER** | `PARTITION BY GLOBAL_ORDER(tenant, user_id)`   | 按 RowKey 自然顺序，Slice 自动切分，支持跨 Slice Scan |

分区列为 RowKey 的前缀连续子集，在 `RowKeyColumn` 中通过 `partition_key` 字段标记。Phase 1 支持 HASH，Phase 2 支持 GLOBAL_ORDER。

### 4.6 路由

Client 通过 ShardManager 缓存分片视图：

```
ShardManager.get("app_db", "user_profile", RowKey("acme", 42))
       │
       ▼  hash("acme:42") % 16 = 3
       │
       ▼
  cache[("app_db", "user_profile", 3)] → {
      slice_id: 1003,
      replicas: [
          {role: LEADER,    host: "us-0:8100"},
          {role: FOLLOWER,  host: "us-1:8100"},
          {role: FOLLOWER,  host: "us-2:8100"},
      ]
  }
```

## 5. 数据流

### 5.1 写路径 (Put)

```
Client                                Master
  │                                      │
  │── 1. GetRoute(ns, table, rowkey) ──────▶│   (缓存命中的话跳过这步)
  │◀──── {SliceId, [Leader, Followers]}─┘
  │
  │── 2. Put(slice, mutation) ──brpc──▶ UnitServer (Leader Replica)
  │                                         │
  │                              ┌──────────┤
  │                              │          ▼
  │                              │    不是 Leader/路由过期?
  │                              │          │
  │                              │    ┌─────┴─────────────┐
  │                              │    ▼                   ▼
  │                              │ NOT_LEADER        SLICE_MOVED
  │                              │ (携带新Leader地址)  (Split/迁移)
  │                              │    │                   │
  │                              │    └──────┬────────────┘
  │                              │           │
  │◀───────── 错误码 ◀────────────┘          │
  │                                          │
  │── 3. 刷新路由缓存 ◀── Master ────────────┘
  │── 4. 重试 Put ──▶ 新 Leader / 新 Slice ──▶ ACK
```

写入保证：

- 一条 braft log entry = 一行 mutation，复制到多数派 Follower 后 commit
- `on_apply()` 原子应用到所有 LG：`lg->memstore.put(key, col, val, ts)`
- **行级事务**：跨多列跨多 LG 的行操作，一条 log entry 原子 apply，无需两阶段提交

### 5.2 读路径 (Get)

```
Client                                Master
  │                                      │
  │── 1. GetRoute(ns, table, rowkey) ──────▶│   (缓存命中的话跳过这步)
  │◀──── {SliceId, [Leader, Followers]}─┘
  │
  │── 2. Get(rowkey) ────brpc───▶ UnitServer
  │                              │
  │                              │ SLICE_MOVED / NOT_LEADER?
  │◀───────── 错误码 ◀───────────┘
  │── 3. 刷新缓存, 重试 ──▶ Row / NOT_FOUND
```

读策略 (客户端配置):

| 策略            | 行为             | 一致性   |
| --------------- | ---------------- | -------- |
| `READ_PRIMARY`  | 总是读 Leader    | 强一致   |
| `READ_FOLLOWER` | 读 Follower 副本 | 最终一致 |
| `READ_NEAREST`  | 读延迟最低的副本 | 最终一致 |

### 5.3 扫描路径 (Scan)

```
Client ── Scan(start_key, end_key, filter) ──brpc──▶ UnitServer

Slice.scan(start, end):
  ├─ 对每个 LG 收集 Iterator: memstore + immutable + sstables
  ├─ 跨 LG 多路归并 (Min-Heap): 同 (rowkey, column) 取最新 timestamp
  ├─ 过滤 tombstone
  └─ 流式返回 (支持分页)
```

Scan 过程中 Slice 发生变化 (Split):
→ UnitServer 返回 SLICE_MOVED + 新 Slice 边界
→ Client 刷新缓存, 以新边界继续扫

### 5.4 Client 路由缓存 & 重试

Client 维护本地 ShardManager，缓存 `(ns, table) → [Slice routes]` 映射。

**UnitServer 返回的错误码**:

| 错误码             | 触发场景                   | 响应携带                  |
| ------------------ | -------------------------- | ------------------------- |
| `NOT_LEADER`       | 请求发到了 braft Follower  | `leader_addr` — 新 Leader |
| `SLICE_MOVED`      | Slice 已迁走 (Split/迁移)  | 受影响 Slice 的最新路由   |
| `KEY_NOT_IN_SLICE` | rowkey 不在当前 Slice 范围 | `hint_slice_id`           |

**重试流程**:

```
Client.put(ns, table, rowkey, mutation):
  │
  ├─ route = ShardManager.get(ns, table, rowkey)    ← 本地缓存
  │
  ├─ retry(3):
  │     ├─ on SUCCESS → return
  │     ├─ on NOT_LEADER:
  │     │     ShardManager.update_leader(slice_id, resp.leader_addr)
  │     │     continue retry (不消耗 Master 往返)
  │     ├─ on SLICE_MOVED / KEY_NOT_IN_SLICE:
  │     │     ShardManager.invalidate(ns, table)
  │     │     ShardManager.reload(ns, table)           ← 从 Master 拉全量
  │     │     continue retry
  │     └─ on RPC error:
  │           ShardManager.invalidate(ns, table)
  │           ShardManager.reload(ns, table)
  │           continue retry
  │
  └─ 重试耗尽 → return ERROR
```

```cpp
class ShardManager {
public:
    std::optional<SliceRoute> get(const std::string& ns,
                                   const std::string& table,
                                   const RowKey& key);
    absl::Status reload(const std::string& ns, const std::string& table);
    void invalidate(const std::string& ns, const std::string& table);
    void update_leader(SliceId slice_id, const std::string& leader_addr);
};
```

## 6. Slice 引擎

每个 Slice 包含多个 LocalityGroup，每个 LG 有独立的 LSM 引擎。WAL (braft log) 在所有 LG 间共享：

```
Slice (braft::StateMachine)
 ├── LG "default"
 │    ├── MemStore        ← SkipList 有序写缓冲
 │    ├── Immutable       ← Flush 中的 MemStore
 │    ├── SSTable L0..Ln  ← 独立 SSTable 文件
 │    └── BlockCache      ← 独立 LRU 缓存
 │
 ├── LG "meta"
 │    ├── MemStore
 │    └── ...
 │
 └── WAL = braft log      ← 所有 LG 共享, 一次 apply, 行级事务
```

### 6.1 Slice 类 (braft::StateMachine)

```cpp
class Slice : public ::braft::StateMachine {
public:
    absl::Status put(const Mutation& mut, google::protobuf::Closure* done);
    absl::StatusOr<std::optional<Row>> get(const RowKey& key, const ReadOptions& opts);
    IteratorPtr scan(const RowKey& start, const RowKey& end, const ScanOptions& opts);

    // braft overrides
    void on_apply(::braft::Iterator& iter) override;
    void on_snapshot_save(::braft::SnapshotWriter* w, ::braft::Closure* done) override;
    int  on_snapshot_load(::braft::SnapshotReader* reader) override;
    void on_leader_start(int64_t term) override;
    void on_leader_stop(const butil::Status& s) override;

private:
    struct LgEngine {
        std::string lg_name;
        sstv2::Schema schema;
        MemStore memstore;
        std::unique_ptr<MemStore> immutable;
        std::vector<SSTableRef> sstables;
        BlockCache block_cache;
    };
    std::vector<LgEngine> lgs_;
    ::braft::Node* node_{nullptr};
    std::unique_ptr<Flusher> flusher_;
    std::unique_ptr<Compactor> compactor_;
};
```

### 6.2 MemStore

```cpp
class MemStore {
public:
    void put(const RowKey& key, const ColumnName& col, const Value& val, Timestamp ts);
    void delete_column(const RowKey& key, const ColumnName& col, Timestamp ts);
    void delete_row(const RowKey& key, Timestamp ts);

    std::optional<Cell> get(const RowKey& key, const ColumnName& col);
    IteratorPtr scan(const RowKey& start, const RowKey& end);
    IteratorPtr snapshot_scan();        // Flush 用一致性快照

    size_t memory_usage() const;
    bool should_flush() const;

private:
    // SkipList<EncodedKey, Cell>
    // EncodedKey = memcomparable(row_keys) + column_name + timestamp
    pl::SkipList<CellKey, CellComparator> list_;
    std::atomic<size_t> memory_bytes_{0};
};
```

并发模型：braft `on_apply` 单线程顺序 apply → writer；读者无锁读 memstore。

Flush 时 swap: `lg.immutable = swap(lg.memstore, new MemStore())` → reader 同时查 memstore + immutable。

### 6.3 写路径详解

```
Slice.put(mutation, done)
  ├─ 1. 验证 (schema check)
  ├─ 2. 序列化 → butil::IOBuf
  └─ 3. braft::Node::apply(task)

                                    ← Leader 复制到多数 Follower

on_apply(iter):
  for each log entry:
    ├─ Parse mutation
    ├─ 按列归属路由: lg->memstore.put(key, col, val, ts)
    └─ 通知 done closure → ACK
```

Mutation 格式 (braft log entry):

```protobuf
message Mutation {
    uint64 slice_id = 1;           uint64 timestamp_us = 2;
    oneof op {
        PutMutation put = 3;
        DeleteColumnMutation del_col = 4;
        DeleteRowMutation del_row = 5;
    }
}
message PutMutation {
    bytes row_key = 1;             // memcomparable 编码
    repeated ColumnUpdate columns = 2;
}
message ColumnUpdate {
    string column_name = 1;        bytes value = 2;
}
message DeleteColumnMutation { bytes row_key = 1; string column_name = 2; }
message DeleteRowMutation    { bytes row_key = 1; }
```

### 6.4 读路径详解

```
Slice.get(rowkey, opts):
  ├─ 按列确定目标 LG
  ├─ lg.memstore.get(key, col)      ← 优先级最高
  ├─ lg.immutable.get(key, col)     ← 正在 Flush
  ├─ lg.block_cache.get(key, col)
  └─ lg.sstables L0→Ln 逐层查找    ← Bloom → Index → Data block
```

Scan 跨 LG 多路归并：

```
Slice.scan(start, end, opts):
  ├─ 对每个 LG: memstore.scan + immutable.scan + sstables[*].scan
  └─ Min-Heap 多路归并: 去重取最新版本, 过滤 tombstone
```

### 6.5 Flush

触发条件 (每个 LG 独立):

| 条件                                        | 动作                     |
| ------------------------------------------- | ------------------------ |
| LG MemStore > `flush_threshold` (默认 64MB) | 触发该 LG Flush          |
| braft log 条数 > `snapshot_log_threshold`   | 所有 LG Flush + Snapshot |
| 手动触发                                    | 指定 LG Flush            |

流程:

```
Flusher:
  ├─ lg.immutable = swap(lg.memstore, new MemStore())
  ├─ sstv2::Builder(lg.schema).add_all(immutable.snapshot_scan()).finish()
  ├─ 写文件到 minidfs: /minitable/data/<ns>/<table>/<slice>/<lg_name>/sst_<seq>.sstv2
  ├─ lg.sstables.push_back(new_sst)
  ├─ 触发 braft snapshot (compact Raft log)
  └─ 释放 lg.immutable
```

### 6.6 Compaction

每个 LG 独立执行 Minor Compaction：

```
触发条件: 某 LG 的 L0 文件数 > 4

Compactor:
  ├─ 选取该 LG L0 的 SSTable
  ├─ 多路归并: MergeIterator → sstv2::Builder(lg.schema)
  │     途中: 物理删除过期 tombstone, 删除超过 MAX_VERSIONS 的旧版本
  ├─ lg.sstables = [new_compacted, remaining...]
  └─ 删除旧 SSTable 文件
```

### 6.7 恢复

**正常重启**:

```
  ├─ braft replay committed log → 所有 LG 的 MemStore 重建
  ├─ 有 snapshot → on_snapshot_load() → 每个 LG 恢复 SSTable 列表
  └─ 就绪
```

**Follower 滞后**: Leader 发 InstallSnapshot RPC → Follower 下载所有 LG 的 sstv2 文件 + meta → on_snapshot_load() → 继续追 log。

### 6.8 SliceSnapshot (Protobuf)

```protobuf
message SliceSnapshot {
    uint64 slice_id = 1;           string table_name = 2;
    bytes  start_key = 3;          bytes  end_key = 4;
    repeated LgSnapshot lg_snapshots = 5;
}
message LgSnapshot {
    string lg_name = 1;
    bytes  schema_blob = 2;        // 该 LG 的 sstv2::Schema 序列化
    repeated SStableFile sstable_files = 3;
}
message SStableFile {
    uint64 seq_num = 1;            string key_file_path = 2;
    string value_file_path = 3;    int64 row_count = 4;
    int64 data_size_bytes = 5;     bytes min_key = 6;
    bytes max_key = 7;
}
```

## 7. Master 设计

Master 自身是一个 3 实例的 braft Raft Group。元信息以 Protobuf 序列化格式持久化在 minidfs 中，不引入外部数据库。

### 7.1 职责

| 模块                | 功能                                                             |
| ------------------- | ---------------------------------------------------------------- |
| **Schema Manager**  | CREATE / ALTER / DROP TABLE；Schema 版本管理                     |
| **Region Manager**  | CREATE / DROP REGION；为 Region 注册/注销 UnitServer             |
| **Slice Scheduler** | 建表时创建 Slice；Slice 分配到 UnitServer；负载均衡              |
| **Route Table**     | 维护 `(ns, table) → [Slice key-range → Replica 地址]` 的路由信息 |
| **UnitServer Mgr**  | UnitServer 注册 + 心跳；故障检测 + 自动 failover                 |

### 7.2 关键流程

**建表**:

```
CREATE TABLE t (...) PARTITION BY HASH(key) SLICES=16 REPLICAS=3

Master:
  1. 验证 Schema，写入 braft log (on_apply 更新 ClusterMeta)
  2. SliceScheduler 创建 Slice + 分配 Replica 到 UnitServer
  3. 通知 UnitServer 启动 braft Node (Slice)
  4. 等待所有 Slice 就绪 → 返回成功
  5. 定期 braft snapshot → cluster_meta.pb 持久化到 minidfs
```

**Split**:

```
Slice 超阈值 → UnitServer 申请 Split → Master 批准新 Slice + 分配
→ UnitServer 在 mid-key 切分 → 子 Slice 各自启动 braft Group → 更新路由表
```

### 7.3 元信息 Schema (ClusterMeta)

```protobuf
message ClusterMeta {
    repeated Region regions = 1;           repeated Table tables = 2;
    repeated Slice slices = 3;             repeated UnitServerInfo unitservers = 4;
}

message Region {
    string name = 1;                       // 集群内唯一, e.g. "prod"
    RegionConfig config = 2;
    RegionState state = 3;                // ACTIVE | DRAINING | OFFLINE
}
message RegionConfig {
    int32 max_tables = 1;                  // 0 = 不限
    int32 max_slices = 2;
}

message Table {
    uint64 table_id = 1;          string namespace = 2;
    string name = 3;              TableSchema schema = 4;
    string region = 5;            TableState state = 6;      // ENABLED | DISABLED | DROPPED
    PartitionSpec partition = 7;
}

message TableSchema {
    repeated RowKeyColumn row_keys = 1;
    repeated LocalityGroup locality_groups = 2;
}
message LocalityGroup {
    string name = 1;                       // e.g. "default"
    repeated ColumnDef columns = 2;
}

enum DataType {
    STRING = 0; INT64 = 1; UINT64 = 2; INT32 = 3; UINT32 = 4;
    BOOL = 5; DOUBLE = 6; FLOAT = 7; BYTES = 8;
    ARRAY = 9; MAP = 10;                   // 仅 ColumnDef 可用
}
message RowKeyColumn {
    string name = 1;          DataType type = 2;         // 0-8, 不可 ARRAY/MAP
    bool partition_key = 3;                              // 前缀连续: 若 true, 前面的列也必须 true
}
message ColumnDef {
    string name = 1;          DataType type = 2;         bool optional = 3;
}

message PartitionSpec {
    PartitionType type = 1;    // HASH | GLOBAL_ORDER
    uint32 slice_count = 2;    // HASH 分片数, GLOBAL_ORDER 时忽略
}

message Slice {
    uint64 slice_id = 1;       uint64 table_id = 2;
    bytes  start_key = 3;      bytes  end_key = 4;
    SliceState state = 5;      // ACTIVE | SPLITTING | MERGING | OFFLINE
    repeated SliceReplica replicas = 6;
}
message SliceReplica {
    uint32 replica_id = 1;     uint64 us_id = 2;
    ReplicaRole role = 3;      // LEADER | FOLLOWER | OFFLINE
}

message UnitServerInfo {
    uint64 us_id = 1;          string region = 2;
    string host = 3;           uint32 port = 4;          int64 capacity = 5;
    UnitServerState state = 6; // LIVE | STALE | DEAD
    int64 last_hb_us = 7;
}
```

### 7.4 变更 & 恢复

变更走 braft log → on_apply 原子替换 ClusterMeta → 定期 snapshot 到 `/minitable/master/cluster_meta.pb`。

Master 启动 → braft replay log → 加载 snapshot → 检查 UnitServer 心跳 → 就绪。

### 7.5 存储布局 (minidfs)

```
/minitable/
├── master/
│   └── cluster_meta.pb           ← Master braft snapshot (braft log 本地, 不在 minidfs)
│
└── data/
    └── <namespace>/
        └── <table_name>/
            └── <slice_id>/
                ├── snapshot_meta.pb  ← Slice braft snapshot
                └── <lg_name>/
                    ├── sst_000001.sstv2
                    ├── sst_000001_value.sstv2
                    └── ...
```

## 8. 模块目录

```
cpp/pl/minitable/
├── docs/
│   └── architecture.md
│
├── proto/
│   ├── master.proto              # MasterService RPC
│   └── unitserver.proto         # UnitServerService RPC
│
├── common/
│   ├── types.h                   # SliceId, Cell, Mutation, Timestamp 等基础类型
│   ├── schema.h                  # TableSchema (wrap sstv2::Schema)

│   └── memstore.h               # MemStore — SkipList 包装, 并发安全
│
├── master/
│   ├── master.h                  # brpc service (DDL, Route, Region 管理)
│   ├── master_sm.h               # braft StateMachine (Master HA)
│   ├── region_mgr.h              # Region CRUD + UnitServer 管理
│   ├── slice_scheduler.h         # Slice 分配 & 负载均衡
│   ├── schema_mgr.h              # Table DDL + Schema 版本管理
│   └── master_main.cpp           # 启动入口
│
├── unitserver/
│   ├── unitserver.h              # brpc service, 托管 N 个 Slice
│   ├── slice.h                   # braft::StateMachine, LSM 状态机
│   ├── slice_writer.cpp          # Put / Delete → braft apply
│   ├── slice_reader.cpp          # Get / Scan 多路归并
│   ├── flusher.h                 # MemStore → sstv2 → braft snapshot
│   ├── compactor.h               # Compaction 调度 & 多路归并
│   ├── block_cache.h             # SSTable block LRU 缓存
│   └── unitserver_main.cpp       # 启动入口
│
└── client/
    ├── client.h                  # 高级 API: put / get / scan / delete
    ├── shard_manager.h           # ShardManager — 路由缓存 + 读策略 + 错误码重试
    └── shell.cpp                 # CLI 交互式客户端
```
