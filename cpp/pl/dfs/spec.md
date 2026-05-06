# C++ HDFS-like 分布式文件系统设计文档

文档版本：v0.1
项目代号：`MiniDFS`
实现语言：C++
元数据后端：MySQL，后续可替换为专用分布式元数据服务
目标用户：个人开发者 / 小团队 / 内部实验性分布式存储项目

---

# 1. 项目背景

本项目希望使用 C++ 实现一个类似 HDFS 的分布式文件系统。系统初期以**可运行、可测试、可演进**为主要目标，不追求完整兼容 Hadoop HDFS 协议。

第一阶段使用 MySQL 作为元数据存储，主要保存：

* 文件和目录元数据；
* block 元数据；
* block replica 分布；
* DataNode 状态；
* lease 信息；
* 操作日志。

未来当系统规模扩大，或者 MySQL 成为瓶颈后，可以将元数据后端替换为 FoundationDB、TiKV、etcd-like KV、自研分布式元数据服务，或者基于 Raft 的专用元数据集群。

项目核心思路：

```text
Client 访问 NameNode 获取元数据；
Client 直接访问 DataNode 读写数据；
NameNode 负责 namespace 和 block 管理；
DataNode 负责本地 block 存储；
MySQL 作为第一版权威元数据存储；
所有元数据访问都通过 MetadataStore 抽象隔离。
```

---

# 2. 设计目标

## 2.1 第一版目标

第一版的目标不是做一个完整的 HDFS，而是做出一个真正可运行的分布式文件系统最小闭环。

第一版需要支持：

1. 启动一个 NameNode；
2. 启动多个 DataNode；
3. DataNode 注册到 NameNode；
4. DataNode 定期心跳；
5. Client 可以创建目录；
6. Client 可以上传本地文件；
7. 文件被切分成 block；
8. block 被写入多个 DataNode；
9. 元数据写入 MySQL；
10. Client 可以读取远端文件；
11. Client 可以列目录；
12. Client 可以删除文件；
13. DataNode 异常时，读请求可以切换副本；
14. 后台可以发现副本不足并补副本。

第一版最重要的闭环是：

```text
mkdir -> create file -> allocate block -> write block replicas
-> commit block -> complete file -> read file -> delete file
```

---

## 2.2 非目标

第一版不做以下能力：

* 不完整兼容 Hadoop HDFS 协议；
* 不支持 POSIX 语义；
* 不支持随机写；
* 不支持文件覆盖写；
* 不支持 append；
* 不支持 snapshot；
* 不支持 erasure coding；
* 不支持 NameNode Federation；
* 不支持复杂权限系统；
* 不支持 Kerberos；
* 不支持跨机房副本策略；
* 不支持 rack awareness；
* 不支持高性能 pipeline write；
* 不支持复杂小文件优化；
* 不支持强一致 FUSE 挂载。

这些能力后续可以做，但第一版不要做。

---

# 3. 核心设计原则

## 3.1 先做 HDFS-like，不做 HDFS-compatible

本项目第一阶段不直接实现 Hadoop HDFS 协议，而是实现自己的 RPC 协议和 Client SDK。

原因：

1. HDFS 协议复杂；
2. Hadoop 生态兼容成本高；
3. 自研阶段需要快速验证核心架构；
4. 先把分布式文件系统基本能力跑通更重要。

后续如果需要对接 Hive、Spark、Trino，可以再实现：

```text
Hadoop FileSystem Adapter
```

或者通过已有协议暴露：

```text
S3-compatible Gateway
```

---

## 3.2 元数据与数据分离

NameNode 只处理元数据和调度，不传输真实文件数据。

```text
错误设计：
Client -> NameNode -> DataNode

正确设计：
Client -> NameNode 获取 block 位置
Client -> DataNode 直接读写数据
```

这样可以避免 NameNode 成为数据带宽瓶颈。

---

## 3.3 MySQL 是权威元数据存储

第一版中：

```text
MySQL metadata = authoritative state
DataNode local block = physical state
```

含义是：

* 文件是否存在，以 MySQL 为准；
* 目录结构，以 MySQL 为准；
* block 属于哪个文件，以 MySQL 为准；
* block 有哪些副本，以 MySQL 为准；
* DataNode 上多余的 block，后台 GC；
* MySQL 记录存在但 DataNode 不存在的副本，后台修正。

---

## 3.4 MetadataStore 必须抽象

不能让 NameNode 业务代码直接拼 SQL。

必须有抽象层：

```cpp
class MetadataStore {
public:
    virtual std::unique_ptr<Transaction> begin() = 0;

    virtual Inode get_inode(uint64_t inode_id) = 0;
    virtual std::optional<Inode> get_child(uint64_t parent_id, std::string_view name) = 0;
    virtual void create_inode(const Inode& inode) = 0;
    virtual void update_inode(const Inode& inode) = 0;

    virtual Block get_block(uint64_t block_id) = 0;
    virtual void create_block(const Block& block) = 0;
    virtual void update_block(const Block& block) = 0;

    virtual std::vector<BlockReplica> get_replicas(uint64_t block_id) = 0;
    virtual void upsert_replica(const BlockReplica& replica) = 0;

    virtual uint64_t alloc_id(IdType type) = 0;

    virtual ~MetadataStore() = default;
};
```

第一版实现：

```text
MySQLMetadataStore
```

未来可以新增：

```text
FDBMetadataStore
TiKVMetadataStore
CustomMetadataStore
```

NameNode 上层逻辑不应该感知底层实现。

---

# 4. 系统总体架构

```text
                          +----------------------+
                          |        Client        |
                          | CLI / SDK / Tooling  |
                          +----------+-----------+
                                     |
                                     | Metadata RPC
                                     |
                          +----------v-----------+
                          |       NameNode       |
                          |----------------------|
                          | NamespaceManager     |
                          | BlockManager         |
                          | DataNodeManager      |
                          | LeaseManager         |
                          | PlacementManager     |
                          | ReplicationManager   |
                          +----------+-----------+
                                     |
                                     | MetadataStore
                                     |
                          +----------v-----------+
                          |        MySQL         |
                          | Metadata Database    |
                          +----------------------+

        Data RPC / Block Transfer
+------------------+     +------------------+     +------------------+
|     DataNode     |     |     DataNode     |     |     DataNode     |
|------------------|     |------------------|     |------------------|
| LocalBlockStore  |     | LocalBlockStore  |     | LocalBlockStore  |
| Heartbeat        |     | Heartbeat        |     | Heartbeat        |
| BlockReport      |     | BlockReport      |     | BlockReport      |
+------------------+     +------------------+     +------------------+
```

---

# 5. 进程模型

## 5.1 NameNode

NameNode 是控制面服务。

职责：

* 管理目录树；
* 管理文件 inode；
* 管理 block；
* 管理 block replica；
* 管理 DataNode 注册和心跳；
* 分配写入 block；
* 返回 block location；
* 检测副本不足；
* 调度副本修复；
* 处理 lease；
* 维护元数据一致性。

---

## 5.2 DataNode

DataNode 是数据面服务。

职责：

* 在本地磁盘保存 block；
* 提供 block 写入接口；
* 提供 block 读取接口；
* 校验 checksum；
* 向 NameNode 注册；
* 向 NameNode 发送 heartbeat；
* 向 NameNode 上报 block report；
* 执行复制任务；
* 执行删除任务。

---

## 5.3 Client

Client 是用户访问入口。

职责：

* 调用 NameNode 创建目录、创建文件、申请 block；
* 直接向 DataNode 写 block；
* 直接从 DataNode 读 block；
* 提供 CLI；
* 后续提供 SDK。

---

# 6. 第一版功能范围

第一版建议命令：

```bash
minidfs format
minidfs namenode --config conf/namenode.yaml
minidfs datanode --config conf/datanode.yaml

minidfs mkdir /tmp
minidfs put ./local.log /tmp/local.log
minidfs get /tmp/local.log ./download.log
minidfs ls /
minidfs stat /tmp/local.log
minidfs rm /tmp/local.log
```

第一版不需要实现完整 Shell。

只要这些命令能稳定工作，就已经形成一个可用闭环。

---

# 7. 目录结构设计

推荐仓库结构：

```text
minidfs/
├── CMakeLists.txt
├── README.md
├── docs/
│   ├── design.md
│   ├── metadata-schema.md
│   ├── rpc-api.md
│   └── roadmap.md
│
├── proto/
│   ├── common.proto
│   ├── namenode.proto
│   └── datanode.proto
│
├── conf/
│   ├── namenode.yaml
│   ├── datanode.yaml
│   └── client.yaml
│
├── sql/
│   └── schema.sql
│
├── src/
│   ├── common/
│   │   ├── status.h
│   │   ├── result.h
│   │   ├── config.h
│   │   ├── logging.h
│   │   ├── time.h
│   │   ├── checksum.h
│   │   └── id_generator.h
│   │
│   ├── metadata/
│   │   ├── metadata_store.h
│   │   ├── mysql_metadata_store.h
│   │   ├── mysql_metadata_store.cpp
│   │   ├── transaction.h
│   │   ├── mysql_transaction.h
│   │   └── mysql_connection_pool.h
│   │
│   ├── namenode/
│   │   ├── namenode_server.h
│   │   ├── namenode_server.cpp
│   │   ├── namespace_manager.h
│   │   ├── namespace_manager.cpp
│   │   ├── block_manager.h
│   │   ├── block_manager.cpp
│   │   ├── datanode_manager.h
│   │   ├── datanode_manager.cpp
│   │   ├── lease_manager.h
│   │   ├── lease_manager.cpp
│   │   ├── placement_manager.h
│   │   ├── placement_manager.cpp
│   │   ├── replication_manager.h
│   │   └── replication_manager.cpp
│   │
│   ├── datanode/
│   │   ├── datanode_server.h
│   │   ├── datanode_server.cpp
│   │   ├── local_block_store.h
│   │   ├── local_block_store.cpp
│   │   ├── heartbeat_sender.h
│   │   ├── heartbeat_sender.cpp
│   │   ├── block_reporter.h
│   │   ├── block_reporter.cpp
│   │   ├── replication_worker.h
│   │   └── replication_worker.cpp
│   │
│   ├── client/
│   │   ├── dfs_client.h
│   │   ├── dfs_client.cpp
│   │   ├── dfs_input_stream.h
│   │   ├── dfs_input_stream.cpp
│   │   ├── dfs_output_stream.h
│   │   └── dfs_output_stream.cpp
│   │
│   └── tools/
│       ├── minidfs_main.cpp
│       ├── namenode_main.cpp
│       ├── datanode_main.cpp
│       └── format_main.cpp
│
├── tests/
│   ├── metadata_store_test.cpp
│   ├── namespace_manager_test.cpp
│   ├── block_manager_test.cpp
│   ├── local_block_store_test.cpp
│   └── integration_test.cpp
│
└── docker/
    ├── docker-compose.yaml
    └── mysql-init.sql
```

---

# 8. 模块详细设计

# 8.1 NamespaceManager

`NamespaceManager` 负责目录和文件命名空间。

核心能力：

```cpp
class NamespaceManager {
public:
    Status mkdir(const MkdirRequest& req, MkdirResponse* resp);

    Status create_file(const CreateFileRequest& req, CreateFileResponse* resp);

    Status get_file_info(const GetFileInfoRequest& req, GetFileInfoResponse* resp);

    Status list_status(const ListStatusRequest& req, ListStatusResponse* resp);

    Status delete_path(const DeleteRequest& req, DeleteResponse* resp);

    Status rename(const RenameRequest& req, RenameResponse* resp);

private:
    StatusOr<Inode> resolve_path(std::string_view path);
    StatusOr<Inode> resolve_parent(std::string_view path);
};
```

第一版可以暂时不做 `rename`，但接口可以预留。

路径解析流程：

```text
输入路径：/a/b/c.txt

1. 从 root inode = 1 开始；
2. 查找 parent_id = 1, name = "a"；
3. 查找 parent_id = inode(a), name = "b"；
4. 查找 parent_id = inode(b), name = "c.txt"；
5. 返回 inode。
```

路径解析依赖索引：

```sql
UNIQUE KEY uk_parent_name (parent_id, name)
```

---

# 8.2 BlockManager

`BlockManager` 负责文件 block 的生命周期。

核心能力：

```cpp
class BlockManager {
public:
    Status allocate_block(const AllocateBlockRequest& req,
                          AllocateBlockResponse* resp);

    Status commit_block(const CommitBlockRequest& req,
                        CommitBlockResponse* resp);

    Status get_block_locations(const GetBlockLocationsRequest& req,
                               GetBlockLocationsResponse* resp);

    Status mark_blocks_deleted(uint64_t inode_id);
};
```

block 状态：

```text
ALLOCATING
COMMITTED
CORRUPT
DELETED
```

block replica 状态：

```text
WRITING
FINALIZED
CORRUPT
STALE
DELETING
DELETED
```

---

# 8.3 DataNodeManager

`DataNodeManager` 负责 DataNode 生命周期。

```cpp
class DataNodeManager {
public:
    Status register_datanode(const RegisterDataNodeRequest& req,
                             RegisterDataNodeResponse* resp);

    Status heartbeat(const HeartbeatRequest& req,
                     HeartbeatResponse* resp);

    std::vector<DataNodeInfo> list_live_datanodes();

    std::vector<DataNodeInfo> choose_datanodes(int replica_num,
                                               uint64_t block_size);
};
```

DataNode 状态：

```text
LIVE
STALE
DEAD
DECOMMISSIONING
DECOMMISSIONED
```

第一版只需要：

```text
LIVE
DEAD
```

---

# 8.4 PlacementManager

`PlacementManager` 负责副本放置策略。

第一版策略：

```text
1. 选择 LIVE DataNode；
2. 过滤 free_bytes < block_size 的节点；
3. 过滤已经包含该 block 的节点；
4. 按 used_ratio 升序；
5. 随机打散；
6. 选择 replication 个节点。
```

接口：

```cpp
class PlacementManager {
public:
    std::vector<DataNodeInfo> choose_targets(
        const std::vector<DataNodeInfo>& candidates,
        int replication,
        uint64_t block_size);
};
```

第一版不做 rack awareness。

---

# 8.5 LeaseManager

`LeaseManager` 用于控制写文件并发。

第一版语义：

```text
一个文件同一时间只能有一个 writer。
```

接口：

```cpp
class LeaseManager {
public:
    Status create_lease(uint64_t inode_id, std::string_view client_id);

    Status check_lease(uint64_t inode_id, std::string_view client_id);

    Status renew_lease(uint64_t inode_id, std::string_view client_id);

    Status close_lease(uint64_t inode_id, std::string_view client_id);

    Status expire_leases();
};
```

第一版可以先简单实现：

* create 文件时创建 lease；
* commit block 时校验 lease；
* complete 文件时关闭 lease；
* 暂不做复杂 lease recovery。

---

# 8.6 ReplicationManager

`ReplicationManager` 负责副本修复。

第一版可以简单做：

```text
每隔 30 秒扫描一次 blocks；
找出 finalized replica 数量 < desired_replica 的 block；
选择新的 DataNode；
下发 replicate block 命令。
```

接口：

```cpp
class ReplicationManager {
public:
    void start();
    void stop();

private:
    void scan_once();
    void schedule_replication(uint64_t block_id);
};
```

---

# 8.7 LocalBlockStore

DataNode 本地 block 存储。

```cpp
class LocalBlockStore {
public:
    Status write_block(uint64_t block_id,
                       uint64_t generation_stamp,
                       const char* data,
                       size_t size);

    Status finalize_block(uint64_t block_id,
                          uint64_t generation_stamp);

    Status read_block(uint64_t block_id,
                      uint64_t offset,
                      uint64_t length,
                      std::string* output);

    Status delete_block(uint64_t block_id);

    StatusOr<BlockLocalInfo> get_block_info(uint64_t block_id);

    std::vector<BlockLocalInfo> scan_all_blocks();
};
```

DataNode 本地目录：

```text
/data/minidfs/
├── current/
│   ├── blk_1001
│   ├── blk_1001.meta
│   ├── blk_1002
│   └── blk_1002.meta
├── tmp/
│   ├── writing_blk_1003
│   └── writing_blk_1003.meta
└── trash/
```

写入流程：

```text
1. 写 tmp/writing_blk_xxx；
2. 写 tmp/writing_blk_xxx.meta；
3. fsync data；
4. fsync meta；
5. rename 到 current/blk_xxx；
6. rename meta 到 current/blk_xxx.meta；
7. 返回成功。
```

---

# 9. 元数据模型

# 9.1 Inode 模型

文件和目录统一用 inode 表示。

```cpp
enum class InodeType {
    DIRECTORY = 1,
    FILE = 2,
};

enum class FileState {
    NORMAL = 0,
    UNDER_CONSTRUCTION = 1,
    DELETED = 2,
};

struct Inode {
    uint64_t inode_id;
    InodeType type;
    uint64_t parent_id;
    std::string name;

    std::string owner;
    std::string group;
    uint32_t permission;

    uint64_t length;
    uint32_t replication;
    uint64_t block_size;

    FileState state;

    uint64_t ctime_ms;
    uint64_t mtime_ms;
    uint64_t version;
};
```

---

# 9.2 Block 模型

```cpp
enum class BlockState {
    ALLOCATING = 0,
    COMMITTED = 1,
    CORRUPT = 2,
    DELETED = 3,
};

struct Block {
    uint64_t block_id;
    uint64_t inode_id;
    uint32_t block_index;
    uint64_t generation_stamp;
    uint64_t length;
    BlockState state;
    uint32_t desired_replica;
    uint64_t ctime_ms;
    uint64_t mtime_ms;
};
```

---

# 9.3 BlockReplica 模型

```cpp
enum class ReplicaState {
    WRITING = 0,
    FINALIZED = 1,
    CORRUPT = 2,
    STALE = 3,
    DELETING = 4,
    DELETED = 5,
};

struct BlockReplica {
    uint64_t block_id;
    uint64_t datanode_id;
    uint64_t storage_id;
    ReplicaState state;
    uint64_t length;
    uint64_t generation_stamp;
    uint64_t report_time_ms;
};
```

---

# 9.4 DataNode 模型

```cpp
enum class DataNodeState {
    LIVE = 0,
    STALE = 1,
    DEAD = 2,
    DECOMMISSIONING = 3,
    DECOMMISSIONED = 4,
};

struct DataNodeInfo {
    uint64_t datanode_id;
    std::string uuid;
    std::string hostname;
    std::string ip;
    uint32_t rpc_port;
    uint32_t data_port;

    std::string rack;

    DataNodeState state;

    uint64_t capacity_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;

    uint64_t last_heartbeat_ms;
};
```

---

# 10. MySQL 表结构

# 10.1 `inodes`

```sql
CREATE TABLE inodes (
    inode_id        BIGINT UNSIGNED NOT NULL PRIMARY KEY,
    type            TINYINT NOT NULL,
    parent_id       BIGINT UNSIGNED NULL,
    name            VARBINARY(255) NOT NULL,

    owner           VARCHAR(64) NOT NULL DEFAULT '',
    group_name      VARCHAR(64) NOT NULL DEFAULT '',
    permission      INT NOT NULL DEFAULT 0755,

    length          BIGINT UNSIGNED NOT NULL DEFAULT 0,
    replication     INT NOT NULL DEFAULT 3,
    block_size      BIGINT UNSIGNED NOT NULL DEFAULT 134217728,

    file_state      TINYINT NOT NULL DEFAULT 0,

    ctime_ms        BIGINT UNSIGNED NOT NULL,
    mtime_ms        BIGINT UNSIGNED NOT NULL,
    version         BIGINT UNSIGNED NOT NULL DEFAULT 0,

    UNIQUE KEY uk_parent_name (parent_id, name),
    KEY idx_parent (parent_id),
    KEY idx_state (file_state)
) ENGINE=InnoDB;
```

根目录初始化：

```sql
INSERT INTO inodes (
    inode_id, type, parent_id, name,
    owner, group_name, permission,
    length, replication, block_size,
    file_state, ctime_ms, mtime_ms, version
) VALUES (
    1, 1, NULL, '',
    'root', 'root', 0755,
    0, 3, 134217728,
    0, 0, 0, 0
);
```

---

# 10.2 `blocks`

```sql
CREATE TABLE blocks (
    block_id          BIGINT UNSIGNED NOT NULL PRIMARY KEY,
    inode_id          BIGINT UNSIGNED NOT NULL,
    block_index       INT UNSIGNED NOT NULL,

    generation_stamp  BIGINT UNSIGNED NOT NULL,
    length            BIGINT UNSIGNED NOT NULL DEFAULT 0,

    state             TINYINT NOT NULL DEFAULT 0,
    desired_replica   INT NOT NULL DEFAULT 3,

    ctime_ms          BIGINT UNSIGNED NOT NULL,
    mtime_ms          BIGINT UNSIGNED NOT NULL,

    UNIQUE KEY uk_file_block_index (inode_id, block_index),
    KEY idx_inode (inode_id),
    KEY idx_state (state)
) ENGINE=InnoDB;
```

---

# 10.3 `block_replicas`

```sql
CREATE TABLE block_replicas (
    block_id          BIGINT UNSIGNED NOT NULL,
    datanode_id       BIGINT UNSIGNED NOT NULL,
    storage_id        BIGINT UNSIGNED NOT NULL,

    state             TINYINT NOT NULL DEFAULT 0,
    length            BIGINT UNSIGNED NOT NULL DEFAULT 0,
    generation_stamp  BIGINT UNSIGNED NOT NULL,
    report_time_ms    BIGINT UNSIGNED NOT NULL,

    PRIMARY KEY (block_id, datanode_id, storage_id),
    KEY idx_datanode (datanode_id),
    KEY idx_state (state)
) ENGINE=InnoDB;
```

---

# 10.4 `datanodes`

```sql
CREATE TABLE datanodes (
    datanode_id       BIGINT UNSIGNED NOT NULL PRIMARY KEY,
    uuid              VARCHAR(128) NOT NULL,
    hostname          VARCHAR(255) NOT NULL,
    ip                VARCHAR(64) NOT NULL,
    rpc_port          INT NOT NULL,
    data_port         INT NOT NULL,

    rack              VARCHAR(255) NOT NULL DEFAULT '/default-rack',

    state             TINYINT NOT NULL DEFAULT 0,

    capacity_bytes    BIGINT UNSIGNED NOT NULL DEFAULT 0,
    used_bytes        BIGINT UNSIGNED NOT NULL DEFAULT 0,
    free_bytes        BIGINT UNSIGNED NOT NULL DEFAULT 0,

    last_heartbeat_ms BIGINT UNSIGNED NOT NULL,

    UNIQUE KEY uk_uuid (uuid),
    KEY idx_state (state),
    KEY idx_heartbeat (last_heartbeat_ms)
) ENGINE=InnoDB;
```

---

# 10.5 `leases`

```sql
CREATE TABLE leases (
    lease_id          BIGINT UNSIGNED NOT NULL PRIMARY KEY,
    inode_id          BIGINT UNSIGNED NOT NULL,
    client_id         VARCHAR(128) NOT NULL,

    state             TINYINT NOT NULL DEFAULT 0,
    active_flag       TINYINT NOT NULL DEFAULT 1,

    expire_time_ms    BIGINT UNSIGNED NOT NULL,
    ctime_ms          BIGINT UNSIGNED NOT NULL,
    mtime_ms          BIGINT UNSIGNED NOT NULL,

    UNIQUE KEY uk_inode_active (inode_id, active_flag),
    KEY idx_expire (expire_time_ms)
) ENGINE=InnoDB;
```

---

# 10.6 `id_allocators`

```sql
CREATE TABLE id_allocators (
    name        VARCHAR(64) NOT NULL PRIMARY KEY,
    next_id     BIGINT UNSIGNED NOT NULL,
    step        BIGINT UNSIGNED NOT NULL
) ENGINE=InnoDB;
```

初始化：

```sql
INSERT INTO id_allocators VALUES ('inode_id', 1000, 1000);
INSERT INTO id_allocators VALUES ('block_id', 1000000, 10000);
INSERT INTO id_allocators VALUES ('lease_id', 1000, 1000);
INSERT INTO id_allocators VALUES ('datanode_id', 1000, 1000);
INSERT INTO id_allocators VALUES ('generation_stamp', 1, 10000);
```

---

# 10.7 `metadata_oplog`

```sql
CREATE TABLE metadata_oplog (
    op_id            BIGINT UNSIGNED NOT NULL PRIMARY KEY,
    op_type          VARCHAR(64) NOT NULL,
    target_inode_id  BIGINT UNSIGNED NULL,
    request_id       VARCHAR(128) NOT NULL,
    payload_json     JSON NOT NULL,
    ctime_ms         BIGINT UNSIGNED NOT NULL,

    UNIQUE KEY uk_request_id (request_id),
    KEY idx_inode (target_inode_id),
    KEY idx_ctime (ctime_ms)
) ENGINE=InnoDB;
```

`metadata_oplog` 第一版可以只写，不消费。后续用于：

* 审计；
* debug；
* 元数据迁移；
* 双写校验；
* 回放恢复。

---

# 11. RPC API 设计

# 11.1 Common

```protobuf
syntax = "proto3";

package minidfs;

message Status {
  int32 code = 1;
  string message = 2;
}

message DataNodeEndpoint {
  uint64 datanode_id = 1;
  string host = 2;
  uint32 data_port = 3;
}

message LocatedBlock {
  uint64 block_id = 1;
  uint64 generation_stamp = 2;
  uint64 offset = 3;
  uint64 length = 4;
  repeated DataNodeEndpoint locations = 5;
}
```

---

# 11.2 NameNodeService

```protobuf
service NameNodeService {
  rpc Mkdir(MkdirRequest) returns (MkdirResponse);
  rpc CreateFile(CreateFileRequest) returns (CreateFileResponse);
  rpc AllocateBlock(AllocateBlockRequest) returns (AllocateBlockResponse);
  rpc CommitBlock(CommitBlockRequest) returns (CommitBlockResponse);
  rpc CompleteFile(CompleteFileRequest) returns (CompleteFileResponse);

  rpc OpenFile(OpenFileRequest) returns (OpenFileResponse);
  rpc ListStatus(ListStatusRequest) returns (ListStatusResponse);
  rpc GetFileInfo(GetFileInfoRequest) returns (GetFileInfoResponse);
  rpc Delete(DeleteRequest) returns (DeleteResponse);

  rpc RegisterDataNode(RegisterDataNodeRequest) returns (RegisterDataNodeResponse);
  rpc Heartbeat(HeartbeatRequest) returns (HeartbeatResponse);
  rpc BlockReport(BlockReportRequest) returns (BlockReportResponse);
}
```

---

# 11.3 DataNodeService

```protobuf
service DataNodeService {
  rpc WriteBlock(stream WriteBlockRequest) returns (WriteBlockResponse);
  rpc ReadBlock(ReadBlockRequest) returns (stream ReadBlockResponse);

  rpc DeleteBlock(DeleteBlockRequest) returns (DeleteBlockResponse);
  rpc ReplicateBlock(ReplicateBlockRequest) returns (ReplicateBlockResponse);
}
```

---

# 12. 核心流程

# 12.1 format

`format` 初始化 MySQL 表和根目录。

流程：

```text
1. 连接 MySQL；
2. 创建 schema；
3. 创建 inodes、blocks、block_replicas 等表；
4. 插入 root inode；
5. 初始化 id_allocators；
6. 完成。
```

命令：

```bash
minidfs format --config conf/namenode.yaml
```

---

# 12.2 DataNode 注册

流程：

```text
1. DataNode 启动；
2. 读取本地配置；
3. 生成或加载 datanode.uuid；
4. 扫描本地磁盘容量；
5. 调用 RegisterDataNode；
6. NameNode 分配 datanode_id；
7. DataNode 保存 datanode_id；
8. 开始 heartbeat；
9. 发送 full block report。
```

---

# 12.3 mkdir

流程：

```text
Client -> NameNode: Mkdir("/warehouse")

NameNode:
1. 解析父目录 "/";
2. 锁定父目录 inode；
3. 检查 warehouse 是否存在；
4. 分配 inode_id；
5. 插入 inodes；
6. 写 metadata_oplog；
7. 提交事务。
```

---

# 12.4 put 文件

假设上传：

```bash
minidfs put ./a.log /warehouse/a.log
```

流程：

```text
1. Client 调用 CreateFile；
2. NameNode 创建 under_construction 文件；
3. Client 本地按 block_size 切分文件；
4. 对每个 block：
   4.1 Client 调用 AllocateBlock；
   4.2 NameNode 选择 DataNode；
   4.3 NameNode 创建 block 和 replica 元数据；
   4.4 Client 向多个 DataNode 写 block；
   4.5 DataNode 写本地 tmp 文件；
   4.6 DataNode finalize block；
   4.7 Client 调用 CommitBlock；
5. 所有 block commit 完成后，Client 调用 CompleteFile；
6. NameNode 将文件状态改为 NORMAL；
7. lease 关闭。
```

第一版写副本方式：

```text
Client 并行写多个 DataNode。
```

暂时不做：

```text
Client -> DN1 -> DN2 -> DN3 pipeline
```

---

# 12.5 get 文件

```bash
minidfs get /warehouse/a.log ./a.log
```

流程：

```text
1. Client 调用 OpenFile；
2. NameNode 返回 file length 和 located blocks；
3. Client 按 block 顺序读取；
4. 每个 block 选择一个 DataNode；
5. 读取失败则切换下一个 replica；
6. 写入本地文件；
7. 本地计算 checksum，验证文件完整性。
```

---

# 12.6 delete 文件

```bash
minidfs rm /warehouse/a.log
```

流程：

```text
1. Client 调用 Delete；
2. NameNode resolve path；
3. NameNode 标记 inode 为 DELETED；
4. NameNode 标记 blocks 为 DELETED；
5. NameNode 标记 block_replicas 为 DELETING；
6. 后台任务下发 DeleteBlock；
7. DataNode 删除本地 block；
8. DataNode 上报删除结果；
9. NameNode 更新 replica 状态为 DELETED。
```

第一版也可以简化：

```text
删除元数据后，DataNode 本地 block 暂不立即删除；
后台 block report 发现 orphan block 后再清理。
```

---

# 13. 本地 block 文件格式

第一版推荐一个简单格式：

```text
blk_<block_id>
blk_<block_id>.meta
```

data 文件只存原始数据。

meta 文件可以是二进制，也可以第一版用文本。

第一版 meta 文本格式：

```text
version=1
block_id=10001
generation_stamp=7
length=134217728
checksum_type=crc32c
chunk_size=1048576
checksum_count=128
checksum_0=...
checksum_1=...
```

第一版也可以只保存整个 block 的 checksum，后续再做 chunk checksum。

推荐路线：

```text
v0.1: block-level checksum
v0.2: chunk-level checksum
```

---

# 14. 配置文件

# 14.1 NameNode 配置

```yaml
server:
  host: "0.0.0.0"
  port: 9000

mysql:
  host: "127.0.0.1"
  port: 3306
  user: "minidfs"
  password: "minidfs"
  database: "minidfs"
  pool_size: 16

filesystem:
  default_replication: 3
  default_block_size: 134217728
  min_write_replica: 2

heartbeat:
  stale_timeout_ms: 30000
  dead_timeout_ms: 600000

replication:
  scan_interval_ms: 30000
  max_replication_tasks_per_round: 100
```

---

# 14.2 DataNode 配置

```yaml
server:
  host: "0.0.0.0"
  data_port: 9100
  rpc_port: 9101

namenode:
  host: "127.0.0.1"
  port: 9000

storage:
  data_dirs:
    - "/tmp/minidfs/dn1"
  reserved_bytes: 1073741824

heartbeat:
  interval_ms: 3000

block_report:
  interval_ms: 600000
```

---

# 14.3 Client 配置

```yaml
namenode:
  host: "127.0.0.1"
  port: 9000

client:
  user: "liubang"
  default_block_size: 134217728
  io_buffer_size: 1048576
```

---

# 15. 第一版实现顺序

这个顺序非常关键。不要从复杂 RPC 和修复逻辑开始。

## 阶段 1：单机元数据闭环

目标：不启动 DataNode，只验证 MySQL 元数据。

任务：

1. 建 C++ 工程；
2. 接入日志；
3. 接入配置；
4. 接入 MySQL connection pool；
5. 实现 `MetadataStore`；
6. 实现 ID allocator；
7. 实现 `mkdir`；
8. 实现 `create_file`；
9. 实现 `list_status`；
10. 实现 `delete`。

验收：

```bash
minidfs format
minidfs mkdir /a
minidfs mkdir /a/b
minidfs ls /
minidfs ls /a
```

---

## 阶段 2：单 DataNode 数据闭环

目标：一个 NameNode，一个 DataNode，单副本写入读取。

任务：

1. 实现 DataNode 本地 block store；
2. 实现 DataNode 注册；
3. 实现 heartbeat；
4. 实现 WriteBlock；
5. 实现 ReadBlock；
6. Client 实现 put；
7. Client 实现 get；
8. block 元数据写入 MySQL。

验收：

```bash
minidfs put ./a.txt /a/b/a.txt
minidfs get /a/b/a.txt ./b.txt
diff ./a.txt ./b.txt
```

---

## 阶段 3：多 DataNode 多副本

目标：支持 3 副本。

任务：

1. NameNode 维护 DataNode 列表；
2. PlacementManager 选择多个 DataNode；
3. Client 并行写多个副本；
4. CommitBlock 记录多个 finalized replica；
5. ReadFile 支持多个 location；
6. 读失败自动切换副本。

验收：

```text
1. 启动 3 个 DataNode；
2. 上传文件；
3. 确认每个 block 有 3 个 replica；
4. kill 一个 DataNode；
5. 仍然可以读取文件。
```

---

## 阶段 4：副本修复

目标：DataNode 挂掉后，系统能自动补副本。

任务：

1. DeadDataNodeScanner；
2. UnderReplicatedBlockScanner；
3. ReplicationTask；
4. DataNode ReplicateBlock；
5. 复制完成后更新 block_replicas。

验收：

```text
1. 3 副本写入文件；
2. kill 一个 DataNode；
3. NameNode 标记副本不足；
4. 启动第 4 个 DataNode；
5. 系统自动补副本；
6. block replica 数量恢复为 3。
```

---

## 阶段 5：稳定性补强

任务：

1. request_id 幂等；
2. lease 超时；
3. block report；
4. orphan block 清理；
5. checksum；
6. metrics；
7. integration test；
8. docker-compose 一键启动。

---

# 16. 关键事务设计

# 16.1 create file 事务

```text
BEGIN

1. SELECT parent inode FOR UPDATE
2. SELECT child WHERE parent_id=? AND name=?
3. child 不存在则继续
4. INSERT inode file_state=UNDER_CONSTRUCTION
5. INSERT lease
6. INSERT metadata_oplog

COMMIT
```

失败场景：

* 目标文件已存在：返回 `AlreadyExists`；
* 父目录不存在：返回 `NotFound`；
* 父路径不是目录：返回 `NotDirectory`。

---

# 16.2 allocate block 事务

```text
BEGIN

1. SELECT inode FOR UPDATE
2. 校验 inode 是 file
3. 校验 file_state = UNDER_CONSTRUCTION
4. 校验 lease active
5. 计算 block_index
6. 分配 block_id
7. 分配 generation_stamp
8. INSERT blocks state=ALLOCATING
9. INSERT block_replicas state=WRITING
10. INSERT metadata_oplog

COMMIT
```

---

# 16.3 commit block 事务

```text
BEGIN

1. SELECT block FOR UPDATE
2. 校验 block state = ALLOCATING
3. 校验成功副本数 >= min_write_replica
4. UPDATE blocks SET state=COMMITTED, length=?
5. UPDATE block_replicas SET state=FINALIZED
6. UPDATE inode length
7. INSERT metadata_oplog

COMMIT
```

---

# 16.4 complete file 事务

```text
BEGIN

1. SELECT inode FOR UPDATE
2. 校验 file_state = UNDER_CONSTRUCTION
3. 校验 lease active
4. 校验所有 block 都是 COMMITTED
5. UPDATE inode SET file_state=NORMAL
6. UPDATE leases SET state=CLOSED, active_flag=0
7. INSERT metadata_oplog

COMMIT
```

---

# 17. 错误码设计

建议统一错误码：

```cpp
enum class ErrorCode {
    OK = 0,

    INVALID_ARGUMENT = 1000,
    NOT_FOUND = 1001,
    ALREADY_EXISTS = 1002,
    NOT_DIRECTORY = 1003,
    IS_DIRECTORY = 1004,
    PERMISSION_DENIED = 1005,

    LEASE_EXPIRED = 2000,
    LEASE_CONFLICT = 2001,
    FILE_UNDER_CONSTRUCTION = 2002,

    NO_AVAILABLE_DATANODE = 3000,
    BLOCK_NOT_FOUND = 3001,
    BLOCK_CORRUPT = 3002,
    REPLICA_NOT_FOUND = 3003,

    MYSQL_ERROR = 4000,
    RPC_ERROR = 5000,
    IO_ERROR = 6000,

    INTERNAL_ERROR = 9000,
};
```

---

# 18. 幂等设计

所有可能重试的接口都应该支持 `request_id`。

例如：

```protobuf
message RequestHeader {
  string request_id = 1;
  string client_id = 2;
  string user = 3;
}
```

需要幂等的请求：

```text
CreateFile
AllocateBlock
CommitBlock
CompleteFile
Delete
WriteBlock
DeleteBlock
ReplicateBlock
```

第一版可以只在 NameNode 层实现 request_id 去重。

方式：

```text
metadata_oplog.request_id 唯一索引
```

如果请求重复：

```text
1. 查询 request_id 对应结果；
2. 返回之前的结果；
3. 或者返回 AlreadyProcessed。
```

MVP 可以先简化为：

```text
重复 request_id 直接返回 OK。
```

后续再做精确结果缓存。

---

# 19. 一致性设计

第一版一致性模型：

```text
1. 元数据强一致；
2. 数据副本最终一致；
3. 文件 close 后可读；
4. under_construction 文件默认不可读；
5. 删除是异步物理删除；
6. 副本修复是异步完成。
```

写成功条件：

```text
min_write_replica = 2
desired_replica = 3
```

含义：

* 写入 2 个副本成功即可 commit；
* 第 3 个副本失败时，后台补齐；
* 如果只成功 1 个副本，则写入失败；
* 失败 block 应该被标记为 stale 或删除。

个人实现时也可以先设置：

```text
min_write_replica = 1
desired_replica = 1
```

等单副本稳定后再开 3 副本。

---

# 20. 部署设计

第一版建议使用 docker-compose：

```yaml
services:
  mysql:
    image: mysql:8.3
    environment:
      MYSQL_ROOT_PASSWORD: root
      MYSQL_DATABASE: minidfs
      MYSQL_USER: minidfs
      MYSQL_PASSWORD: minidfs
    ports:
      - "3306:3306"

  namenode:
    image: minidfs:latest
    command: ["minidfs-namenode", "--config", "/conf/namenode.yaml"]
    depends_on:
      - mysql
    ports:
      - "9000:9000"

  datanode1:
    image: minidfs:latest
    command: ["minidfs-datanode", "--config", "/conf/datanode1.yaml"]
    depends_on:
      - namenode
    ports:
      - "9100:9100"
      - "9101:9101"

  datanode2:
    image: minidfs:latest
    command: ["minidfs-datanode", "--config", "/conf/datanode2.yaml"]
    depends_on:
      - namenode
    ports:
      - "9200:9100"
      - "9201:9101"

  datanode3:
    image: minidfs:latest
    command: ["minidfs-datanode", "--config", "/conf/datanode3.yaml"]
    depends_on:
      - namenode
    ports:
      - "9300:9100"
      - "9301:9101"
```

---

# 21. 测试计划

## 21.1 单元测试

必须覆盖：

```text
MetadataStoreTest
  - create inode
  - get child
  - list children
  - create block
  - update replica

NamespaceManagerTest
  - mkdir
  - create file
  - path resolve
  - delete
  - duplicate create

BlockManagerTest
  - allocate block
  - commit block
  - get locations

LocalBlockStoreTest
  - write block
  - read block
  - finalize block
  - delete block
  - restart scan
```

---

## 21.2 集成测试

测试场景：

```text
1. 单副本 put/get；
2. 三副本 put/get；
3. kill 一个 DataNode 后读取；
4. 上传大文件，多 block；
5. 删除文件后 list 不可见；
6. DataNode 重启后 block report；
7. NameNode 重启后仍能读取文件；
8. 写入过程中 DataNode 失败；
9. 写入过程中 Client 失败；
10. 重复请求幂等。
```

---

## 21.3 压力测试

第一版简单做：

```bash
for i in $(seq 1 1000); do
  minidfs put ./small_file /bench/file_$i
done
```

测试指标：

```text
create QPS
put 吞吐
get 吞吐
NameNode RPC latency
MySQL query latency
DataNode disk write throughput
```

---

# 22. 可观测性

## 22.1 日志

建议日志格式：

```text
[2026-05-06 18:00:00.123] [INFO] [namenode] request_id=xxx op=create path=/a/b.txt inode_id=1001 cost_ms=4
[2026-05-06 18:00:01.456] [INFO] [datanode] op=write_block block_id=2001 length=134217728 cost_ms=120
```

不要第一版就强制 JSON 日志。

---

## 22.2 Metrics

NameNode metrics：

```text
namenode_rpc_total
namenode_rpc_latency_ms
namenode_mysql_query_latency_ms
namenode_live_datanodes
namenode_dead_datanodes
namenode_under_replicated_blocks
namenode_missing_blocks
namenode_files_total
namenode_blocks_total
```

DataNode metrics：

```text
datanode_write_bytes_total
datanode_read_bytes_total
datanode_write_latency_ms
datanode_read_latency_ms
datanode_disk_used_bytes
datanode_disk_free_bytes
datanode_blocks_total
datanode_failed_write_total
datanode_failed_read_total
```

---

# 23. 后续演进路线

## 23.1 v0.1：单副本最小系统

能力：

* MySQL 元数据；
* 单 NameNode；
* 单 DataNode；
* mkdir；
* put；
* get；
* ls；
* rm。

---

## 23.2 v0.2：多副本

能力：

* 多 DataNode；
* block replica；
* 读副本切换；
* 写多个副本；
* DataNode heartbeat。

---

## 23.3 v0.3：自动修复

能力：

* dead DataNode 检测；
* under-replication 扫描；
* ReplicateBlock；
* DeleteBlock；
* block report。

---

## 23.4 v0.4：稳定性

能力：

* lease recovery；
* request idempotency；
* checksum；
* metrics；
* admin command；
* docker-compose；
* integration test。

---

## 23.5 v1.0：可试用版本

能力：

* NameNode active-standby；
* MySQL leader lease；
* DataNode decommission；
* 简单权限；
* 大目录分页；
* pipeline write；
* 更完整的客户端库。

---

## 23.6 v2.0：替换元数据服务

能力：

* 抽象 MetadataStore 完整落地；
* 新增分布式 MetadataStore；
* MySQL oplog mirror；
* 双写；
* shadow read；
* 切主；
* MySQL 降级为备份或审计库。

---

# 24. 主要风险

## 24.1 过早做复杂功能

最大风险是你一开始就做：

* HA；
* pipeline；
* snapshot；
* HDFS 兼容；
* FUSE；
* 权限系统；
* erasure coding。

这些都会拖慢第一版闭环。

建议先坚持：

```text
单副本跑通 -> 多副本跑通 -> 修复跑通 -> 再谈高级功能。
```

---

## 24.2 MySQL 元数据瓶颈

MySQL 初期足够用，但未来会遇到：

* path resolve QPS 高；
* block report 写入量大；
* 大目录 list 慢；
* 单表数据膨胀；
* NameNode 多实例缓存一致性问题。

所以必须保留：

```text
MetadataStore 抽象
metadata_oplog
ID allocator
inode 模型
```

这些会让后续迁移更容易。

---

## 24.3 本地 block 状态和 MySQL 不一致

典型问题：

```text
MySQL 认为 block 存在，但 DataNode 本地不存在；
DataNode 本地有 block，但 MySQL 没有记录；
replica length 不一致；
checksum 不一致；
DataNode 写一半崩溃。
```

解决方式：

```text
1. 写 tmp 文件；
2. finalize 后再 commit；
3. DataNode 启动时扫描本地 block；
4. 定期 block report；
5. NameNode 后台 reconcile；
6. orphan block 延迟删除。
```

---

# 25. 最终建议

这个项目你自己动手写的话，建议第一版严格控制成：

```text
一个 C++ 写的、MySQL 做元数据的、支持多 DataNode 多副本的大文件分布式存储系统。
```

第一阶段不要追求“像不像真正的 HDFS”，而是追求下面这个闭环稳定：

```text
启动集群
-> 创建目录
-> 上传大文件
-> 写入多个 DataNode
-> 元数据落 MySQL
-> 下载文件
-> 校验一致
-> kill DataNode
-> 仍然可读
-> 自动补副本
```

只要这个闭环完成，后续你就有了一个真正能演进的系统骨架。

下一步最适合先写的是：

```text
1. schema.sql
2. MetadataStore 接口
3. MySQLMetadataStore
4. NamespaceManager
5. LocalBlockStore
6. 单副本 put/get
```

把这 6 个模块写完，这个项目就从“设计”进入“系统雏形”阶段了。

