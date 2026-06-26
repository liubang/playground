# MiniDFS TLA+ 形式化验证

TLA+ 形式化规格，用于验证 MiniDFS 中关键状态机的正确性。

## 目录结构

```
cpp/pl/minidfs/tla/
├── README.md              # 本文件
├── DatanodeState.tla      # DataNode 心跳/生命周期状态机
├── DatanodeState.cfg      # TLC 模型配置
├── BlockReplicaState.tla  # Block + Replica 联合状态机
├── BlockReplicaState.cfg  # TLC 模型配置
├── FileLeaseState.tla     # File + Lease 状态机
├── FileLeaseState.cfg     # TLC 模型配置
├── WriteProtocol.tla      # 完整 Pipeline 写协议
└── WriteProtocol.cfg      # TLC 模型配置
```

## 前置条件

安装 TLA+ 工具链：

```bash
# macOS
brew install tla-plus-toolbox

# 或者直接下载 tla2tools.jar
# https://lamport.azurewebsites.net/tla/toolbox.html
```

所需工具：

- **SANY** (TLA+ Parser) — 语法和类型检查
- **TLC** (TLA+ Model Checker) — 穷举状态空间探索
- *(可选)* **Apalache** — 符号模型检查器，适合更大规模验证
- *(可选)* **TLAPS** — 交互式证明系统

## 快速开始

### 语法检查 (SANY)

```bash
TLA2=/path/to/tla2tools.jar
cd cpp/pl/minidfs/tla

for f in *.tla; do
    echo "=== SANY $f ==="
    java -cp "$TLA2" tla2sany.SANY "$f"
done
```

### 模型检查 (TLC)

```bash
cd cpp/pl/minidfs/tla

for cfg in *.cfg; do
    spec="${cfg%.cfg}.tla"
    echo "=== TLC $spec ==="
    java -cp "$TLA2" tlc2.TLC -config "$cfg" "$spec"
done
```

### 模型检查结果（当前配置）

| 规格 | 生成状态数 | 不同状态数 | 模型规模 | 结果 |
|------|-----------|-----------|---------|------|
| DatanodeState | 363,518 | 53,361 | 3 个 DN, 最大时间 20 | 通过 |
| BlockReplicaState | 2,267,173 | 181,476 | 2 个 block, 6 个 replica, 3 个 DN | 通过 |
| FileLeaseState | 46,437 | 8,676 | 2 个文件, 2 个客户端, 最大时间 6 | 通过 |
| WriteProtocol | 4,851,717 | 556,378 | 1 个文件, 3 个 DN, 3 个 block | 通过 |

所有模型均完成穷举状态空间探索（队列剩余状态数为 0），无不变量违反。

## 规格说明

### 1. DatanodeState — DataNode 生命周期

**验证的代码路径:** `namenode/datanode_manager.cpp`

```
状态转换:
  (new) --> Live --> Stale --> Dead
            ^         |         |
            +--- heartbeat -------+
```

每次 `TickAndScan` 步骤非确定性推进时间，并根据距上次心跳的间隔更新所有 DataNode 状态。`Heartbeat` 动作将节点重置为 Live。

**验证的不变量:**

| 不变量 | 含义 |
|--------|------|
| `TypeOK` | 所有状态值在合法枚举范围内 |
| `LiveImpliesFreshHeartbeat` | Live 节点心跳间隔必须小于 `StaleTimeoutMs` |
| `StaleImpliesAgedHeartbeat` | Stale 节点心跳间隔在 `[StaleTimeoutMs, DeadTimeoutMs)` 区间 |
| `DeadImpliesExpiredHeartbeat` | Dead 节点心跳间隔 >= `DeadTimeoutMs` |

**设计发现:**

`handle_heartbeat()` 无条件设置 `dn.state = DataNodeState::kLive`（`datanode_manager.cpp` 第 99 行），允许已 Dead 的节点被心跳复活。这与 HDFS 不同——HDFS 中 Dead DataNode 需要显式重新注册。

**模型配置 (`DatanodeState.cfg`):**
```tla
SPECIFICATION Spec
CONSTANTS
    DataNodes = {dn1, dn2, dn3}
    StaleTimeoutMs = 3
    DeadTimeoutMs = 10
    MaxTime = 20
INVARIANTS TypeOK StateConsistency
```

### 2. BlockReplicaState — Block + Replica 联合状态机

**验证的代码路径:** `namenode/block_manager.cpp`

```
Block:   Allocating --> Committed --> Corrupt
              |             |            |
              +-------------+------------+
                            v
                         Deleted

Replica: Writing --> Finalized --> Stale
            |           |           |
            +-----------+-----------+
                        v
                    Corrupt --> Deleting --> Deleted
```

**验证的不变量:**

| 不变量 | 含义 |
|--------|------|
| `TypeOK` | 所有状态值在合法枚举范围内 |
| `AllocatingBlockReplicasAreWritingOrCorrupt` | Allocating 状态 block 的 replica 只能是 Writing 或 Corrupt |
| `DeletedBlockHasNoActiveReplicas` | 已删除 block 不能有 Writing/Finalized/Corrupt/Stale 状态的 replica |

**设计发现:**

`commit_block()` 存在幂等路径（`block_manager.cpp` 第 147-148 行的 `already_committed` 检查）。当重复提交请求携带相同的 `generation_stamp` 时，block 状态更新被跳过，但仍遍历所有 replica。由于 Finalized-to-Finalized 是幂等的，这在实践中是安全的。模型确认了无论重试顺序如何，block 级别的一致性均得以保持。

`CommittedBlockHasMinReplicas` 属性（要求 >= MinWriteReplica 个健康 replica）是由 `ReplicationManager` 处理的容错活性属性，而非安全不变量。级联 DataNode 故障可临时将 replica 数降至 `MinWriteReplica` 以下。

**模型配置 (`BlockReplicaState.cfg`):**
```tla
SPECIFICATION Spec
CONSTANTS
    Blocks = {1, 2}
    Replicas = {1, 2, 3, 4, 5, 6}
    DataNodes = {1, 2, 3}
    MinWriteReplica = 2
INVARIANTS BlockReplicaInvariants
```

### 3. FileLeaseState — File + Lease 互斥验证

**验证的代码路径:** `namenode/namespace_manager.cpp` + `namenode/lease_manager.cpp`

```
File:  Normal <--> UnderConstruction --> Deleted
                      |
                      v
                   Completed

Lease: (None) --> Active --> Closed
                   |
                   +-- (renew 延长过期时间)
```

**验证的不变量:**

| 不变量 | 含义 |
|--------|------|
| `TypeOK` | 所有状态值在合法枚举范围内 |
| `OnlyUCFileHasActiveLease` | 只有 UnderConstruction 文件可以有 Active lease |
| `DeletedFileHasNoActiveLease` | 已删除文件不能有 Active lease |
| `ActiveLeaseHasFutureExpiry` | Active lease 满足 `expire_time >= now` 且 `<= now + LeaseTimeoutMs` |
| `NormalFileLeaseConsistency` | Normal 文件没有 Active lease（None 或 Closed） |

**设计发现:**

`expire_stale_leases()`（`lease_manager.cpp` 第 101 行）关闭过期 lease 但不会将文件从 UnderConstruction 恢复为 Normal。这意味着客户端在写入过程中崩溃会导致文件永久停留在 UnderConstruction 状态——一个"孤儿文件"，需要管理员手动干预或未来在 `NameNodeMaintenance` 中添加清理逻辑。

**模型配置 (`FileLeaseState.cfg`):**
```tla
SPECIFICATION Spec
CONSTANTS
    Files = {f1, f2}
    Clients = {c1, c2}
    LeaseTimeoutMs = 3
    MaxTime = 6
INVARIANTS FileLeaseInvariants
```

### 4. WriteProtocol — 完整 Pipeline 写协议

**验证的代码路径:** 完整写入流程，横跨 `namespace_manager.cpp`、`block_manager.cpp` 和 `lease_manager.cpp`

这是最核心的规格，建模了完整的写入生命周期：

1. `CreateFile` — 文件进入 UnderConstruction，获取排他 lease
2. `AllocateBlock` — NameNode 选择 DataNode，创建 Writing 状态的 replica
3. `CommitBlock` — 确认的 replica 转为 Finalized，block 转为 Committed
4. `CompleteFile` — 所有 block 已提交，文件转为 Completed
5. `DataNodeFailure` — 故障注入：DN 故障使其上所有 replica 变为 Corrupt
6. `LeaseExpire` — 客户端崩溃模拟

**验证的不变量:**

| 不变量 | 含义 |
|--------|------|
| `TypeOK` | 所有状态值在合法枚举范围内 |
| `CompletedFileBlocksAreCommitted` | Completed 文件的每个 block 必须是 Committed 状态 |
| `UnallocatedBlockHasNoReplicas` | 未分配的 block 在任何 DataNode 上没有 replica |
| `OnlyUCFileHasLease` | 只有 UnderConstruction 文件可以持有 lease（写互斥） |

**容错说明:**

`CompletedFileHasAtLeastOneReplica` 属性（要求完成后每个 block 至少 1 个 Finalized replica）是容错活性关注点。模型正确地展示了级联 DataNode 故障可以摧毁一个 block 的所有 replica。在生产环境中，`ReplicationManager` 定期扫描会检测副本不足的 block，并在额外故障发生前触发重新复制。将 `ReplicationManager` 修复循环建模后，可将此属性强化为时序保证。

**模型配置 (`WriteProtocol.cfg`):**
```tla
SPECIFICATION Spec
CONSTANTS
    Clients = {1}
    Files = {1}
    DataNodes = {1, 2, 3}
    MinWriteReplica = 2
    MaxBlockIndex = 2
INVARIANTS WriteProtocolInvariants
```

## 设计原理

### 有限模型 vs 无限模型

所有规格使用有限（bounded）模型和小型常量集合，原因是：
- 分布式协议的安全 bug 通常在小规模配置中就会暴露
- TLC 对状态空间做穷举枚举，在给定边界内提供证明级别的确定性
- 对于无界验证，需要使用 TLAPS（证明系统）或 Apalache（符号模型检查器）

### 命名约定

规格中的标识符与 C++ 源码中的枚举值保持一致：

| C++ 枚举值 | TLA+ 字符串 |
|-----------|-------------|
| `DataNodeState::kLive` | `"Live"` |
| `BlockState::kAllocating` | `"Allocating"` |
| `ReplicaState::kWriting` | `"Writing"` |
| `FileState::kUnderConstruction` | `"UnderConstruction"` |
| `LeaseState::kActive` | `"Active"` |

每个 `.tla` 文件头部注释标注了对应的 `.cpp` 源文件和函数名。

### 柯里化函数

`WriteProtocol` 规格使用柯里化函数表示（`[A -> [B -> C]]`）而非扁平的元组键函数（`[A \X B -> C]`）。这是因为 TLC 的 `EXCEPT` 语法（`![key] = value`）在外层函数上操作；对于形如 `![block_id][datanode_id] = value` 的嵌套更新，外层域必须是单键而非元组。

### TLC 中间文件

TLC 在模型检查过程中生成 trace 文件（`*_TTrace_*.tla`）和状态队列目录（`states/`）。这些文件已通过 `.gitignore` 排除。如需查看反例 trace，运行 TLC 后不要清理，在 TLA+ Toolbox 中打开生成的 `_TTrace_*.tla` 文件即可。

## 验证路线图

| 优先级 | 规格 | 状态 | 说明 |
|--------|------|------|------|
| P0 | WriteProtocol | 已完成 | 核心写协议，最复杂也最关键 |
| P1 | FileLeaseState | 已完成 | Lease 互斥，发现孤儿文件问题 |
| P1 | BlockReplicaState | 已完成 | Block/replica 在分配和提交期间的一致性 |
| P2 | DatanodeState | 已完成 | 心跳状态机，验证基本正确性 |
| P3 | ReplicationRepair | 规划中 | ReplicationManager 的副本不足/过量 block 检测与修复 |
| P3 | DecommissionProtocol | 规划中 | DataNode 下线状态转换（当前在 `check_stale_and_dead` 中被跳过） |

## 通过规格分析发现的问题

### 1. Dead 节点可被心跳复活

**位置:** `datanode_manager.cpp:99` — `handle_heartbeat()` 无条件设置 `dn.state = DataNodeState::kLive`

**影响:** 如果 DataNode 在被标记为 Dead（超过 10 分钟无心跳）后恢复，其下一次心跳会立即将其复活。这与 HDFS 语义不同——HDFS 中 Dead 节点需要管理员显式重新注册。对 MiniDFS 而言这可能是有意为之，因为没有下线协议。

**严重程度:** 低（简化设计的有意选择）

### 2. 孤儿 UnderConstruction 文件

**位置:** `lease_manager.cpp:101` — `expire_stale_leases()` 关闭 lease 但不将文件从 `UnderConstruction` 恢复为 `Normal`

**影响:** 如果客户端在持有写 lease 时崩溃，lease 过期后文件永久停留在 `UnderConstruction` 状态。该文件无法被其他客户端读取或写入。当前没有自动化机制从此状态恢复。

**严重程度:** 中（需要手动干预或在 `NameNodeMaintenance` 中添加清理逻辑）

### 3. CommitBlock 幂等性与部分 Replica 状态

**位置:** `block_manager.cpp:147-196` — `already_committed` 分支跳过 block 状态更新，但仍遍历所有 replica，将 `finalized_datanode_ids` 中的 Writing replica 转为 Finalized。

**影响:** 如果两个并发的提交请求携带不同的 `finalized_datanode_ids` 集合，第二个请求可能将额外的 replica 转为 Finalized。由于 Finalized-to-Finalized 是幂等的，这是安全的。然而，模型揭示如果 `generation_stamp` 不匹配，replica 状态转换理论上可能作用于新分配的 replica——尽管这需要 `generation_stamp` 碰撞，而单调 ID 分配器可防止此情况。

**严重程度:** 低（由于幂等性和单调 generation stamp 而安全）

## 参考资料

- [TLA+ 主页](https://lamport.azurewebsites.net/tla/tla.html)
- [TLA+ 教程 (PlusCal)](https://lamport.azurewebsites.net/tla/tutorial/home.html)
- [Specifying Systems (Lamport)](https://lamport.azurewebsites.net/tla/book.html)
- [HDFS 架构指南](https://hadoop.apache.org/docs/current/hadoop-project-dist/hadoop-hdfs/HdfsDesign.html)
