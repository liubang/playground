# MiniVessel 共享 WAL 内存副本 HA Runtime

## 1. 文档状态

本文描述 `cpp/pl/minivessel` **当前已经实现的协议、公开接口、使用方法和能力边界**，并单独列出尚未实现的生产能力。代码与本文冲突时以代码和测试为准。

MiniVessel core 的 `FramedSharedWal` 可用于单机确定性测试；`e2e/` 另有真实多进程验证架构：独立 shared-log authority、三个 Counter replica 与 MiniDFS immutable objects。E2E authority 的 catalog/epoch/lease 只在内存中，明确不是生产 HA authority；authority 在验收期间不重启，catalog 无持久化和容量治理。每个已发布 record 的 payload 都写入配置为三副本的 MiniDFS，验收逐对象检查 NameNode 当时全部 block 的静态 3/3 副本报告；replica 重启会通过 `FileIdentity` 绑定读取并 replay。测试开始要求洁净 authority/replica 状态，结束要求 9 个容器仍健康。fencing 来自存活的 shared-log authority，不是 MiniDFS storage fencing，也不证明 authority crash 后仍能 fencing 或恢复 catalog。

## 2. 定位

MiniVessel 是基于共享 WAL 的业务内存状态机 Primary/Standby HA Runtime，不是存储复制系统，也不负责复制业务数据文件。

业务接入只需：

1. 实现 `ReplicatedStateMachine`；
2. 为所有副本装配同一个逻辑 `SharedWal`；
3. 周期驱动 `ReplicaRuntime::poll()`；
4. 由外部选主结果创建或释放 RAII `PrimarySession`；
5. 仅向 `RuntimeRole::kPrimary` 副本提交 mutation。

MiniVessel 负责：

- WriterEpoch fencing；
- LRSN 分配与连续性校验；
- WAL frame 编解码、CRC32C 和 durable prefix；
- Primary WAL-first apply；
- Standby tail/replay；
- promotion catch-up 和 `PrimaryBarrier`；
- 简化的 inline checkpoint record；
- append/sync 不确定结果时关闭写入准入。

MiniVessel 当前不负责：

- 业务数据文件复制；
- 多数派 consensus；
- 外部副作用 exactly-once；
- 自动成员管理和自动选主；
- request-id durable dedupe；
- WAL segment、group commit、retention/GC；
- 后台线程、资源调度和多租户 QoS。

## 3. 架构

```text
                       shared durable prefix
                +--------------------------------+
                |             SharedWal          |
                | LRSN / WriterEpoch / CRC / sync|
                +---------------+----------------+
                                |
                +---------------+----------------+
                |                                |
        +-------v--------+               +-------v--------+
        | ReplicaRuntime |               | ReplicaRuntime |
        | Primary        |               | Standby        |
        +-------+--------+               +-------+--------+
                |                                |
        +-------v--------+               +-------v--------+
        | Business SM A  |               | Business SM B  |
        | in-memory state|               | in-memory state|
        +----------------+               +----------------+
```

模块依赖：

```text
types
  -> filesystem
       -> local_filesystem
       -> minidfs_filesystem
  -> shared_wal
       -> replica_runtime
            -> example/counter
```

### 3.1 核心组件

| 组件 | 职责 |
|---|---|
| `MonotonicId` / `Lrsn` | 用强类型隔离不同协议序号 |
| `ObjectMetadataBackend` | 只定义 immutable checkpoint/sealed object 存储原语 |
| `ActiveLogStorage` | 独立定义 authoritative active log 的 append/fencing/durable-tail 原语 |
| `FramedSharedWal` | 仅依赖 `ActiveLogStorage`，编码 framed WAL 并校验 durable prefix |
| `ReplicaRuntime` | 编排角色、session/token、tail/apply、submit、read guard 和 checkpoint |
| `ReplicatedStateMachine` | 业务确定性状态转换与 checkpoint 编解码 |
| `ApplyResult` / `ApplyContext` | 区分业务拒绝与系统错误，以及 Primary commit 与 WAL replay |

## 4. 核心术语

- **GroupIdentity**：`(group_id, incarnation)`；删除重建 group 时必须改变 incarnation。
- **AssignmentEpoch**：外部 assignment 的单调版本，旧 assignment 不得重新获取 writer。
- **WriterEpoch**：存储后端分配的单调 fencing token。新的 writer 必须让旧 writer 的 append/sync 失败。
- **LRSN**：Log Record Sequence Number。一个 WAL lineage 内从 1 开始严格连续的逻辑记录序号，C++ 类型为 `Lrsn`。
- **DurableLRSN**：共享 WAL 已发布的最大完整 record LRSN。
- **AppliedLRSN**：副本状态机已成功处理的最大连续 LRSN。
- **PrimaryBarrier**：新 WriterEpoch 完成 recovery 后写入的权威任期边界，不属于业务 mutation。
- **durable offset**：存储后端已经 fsync 并正式发布的字节边界；reader 不得读取该边界之后的数据。

代码、测试、日志与本文统一使用 `Lrsn`、`lrsn`、`DurableLRSN`、`AppliedLRSN`，不使用其他简称。

## 5. 当前协议不变量

当前实现强制以下不变量：

1. `FramedSharedWal` 只允许持有有效 writer session 时 append。
2. 每条 record 同时携带有效的 `Lrsn` 和 `WriterEpoch`。
3. LRSN 从 1 开始严格连续；gap、duplicate、损坏 frame 均 fail-closed。
4. Primary 只有在 append + sync 成功并验证 durable proof 后才调用业务 `apply()`。
5. Primary 在业务 `apply()` 返回 applied 或 deterministic rejected 后均推进 AppliedLRSN；只有系统 `Status` 错误才使副本失败。
6. Standby 只读取 backend 发布的 durable prefix，并按 LRSN 顺序 replay。
7. promotion 固定执行：获取 writer/fence 旧主 → catch-up → durable barrier → apply barrier → Primary。
8. `PrimaryBarrier` 只推进 AppliedLRSN，不传入业务状态机。
9. `apply()` 的系统错误使副本进入 `kFailed`；业务拒绝作为确定性结果推进 LRSN，不得在 replay 时改变判定。
10. append/sync 失败的 durability 结果按 uncertain 处理：关闭 admission、释放 writer、回到 Standby。
11. sync 返回的 WriterEpoch 必须等于当前 epoch，durable offset 必须恰好覆盖本次完整 frame。
12. LRSN、packet sequence 或 byte offset 溢出时停止追加，不允许回绕。

当前**尚未保证**：

- 同一个 `request_id` 重试只产生一次逻辑效果；
- 响应丢失后的 exactly-once；
- checkpoint 后跳过旧 WAL 的快速恢复；
- writer lease 在没有周期 `poll()` 时自动续租；
- LocalFS 上过期且仍存活进程的强制接管；
- MiniDFS 上跨机器 active WAL fencing。

因此当前 mutation API 的重试语义是 **at-least-once 风险模型**：成功返回表示当前调用已 durable 且已 apply；错误可能表示记录未写入，也可能表示已 durable 但响应未知。业务在引入 durable dedupe 前必须自行保证 mutation 幂等。

## 6. WAL 逻辑与物理格式

### 6.1 记录类型

```cpp
enum class LogRecordType : uint8_t {
    kMutation = 1,
    kPrimaryBarrier = 2,
    kCheckpoint = 3,
};
```

`LogRecord` 包含：

- `lrsn`
- `writer_epoch`
- `type`
- `request_id`
- opaque `payload`

### 6.2 当前 frame 格式

所有整数使用 little-endian。当前是固定 40 字节 header 加变长 body：

```text
offset  size  field
0       4     magic = 0x4d56574c (MVWL)
4       2     frame version = 1
6       1     record type
7       1     reserved = 0
8       8     LRSN
16      8     WriterEpoch
24      4     request-id bytes
28      4     payload bytes
32      4     CRC32C
36      4     reserved = 0
40      N     request id
40+N    M     payload
```

CRC 字段计算时先置零，CRC32C 覆盖完整 frame。decoder 在分配前检查长度和 `max_record_bytes`，默认单 record body 上限为 16 MiB。

当前 WAL 是单文件实现，没有 segment、footer、sparse index、batch 和 padding。`read()` / `durable_lrsn()` 当前会扫描整个 durable prefix，适合功能验证，不适合无限增长的生产 WAL。

### 6.3 append durability proof

`FramedSharedWal::append()` 执行：

1. 用当前 `next_lrsn` 和 WriterEpoch 构造 record；
2. 编码 frame；
3. 按 expected byte offset 和 packet sequence append；
4. 调用 backend `sync()`；
5. 校验 sync 返回的 WriterEpoch；
6. 校验 durable offset 等于本次 frame 尾部；
7. 推进下一 LRSN 和 packet sequence；
8. 返回 `DurableAppend`。

任何 append/sync 错误都不能证明本次请求未 durable。调用方必须关闭当前 writer session 并重新扫描 durable prefix。

## 7. ReplicaRuntime

### 7.1 配置

```cpp
struct ReplicaRuntimeOptions {
    std::string replica_id;
    AssignmentEpoch assignment_epoch;
    uint64_t writer_lease_timeout_ms = 30'000;
    size_t tail_batch_records = 128;
    RoleLifecycle* role_lifecycle = nullptr;
};
```

`SharedWal` 和 `ReplicatedStateMachine` 由调用方持有，生命周期必须长于 `ReplicaRuntime`。

### 7.2 角色

当前 runtime 使用精简角色：

```text
Standby -> Promoting -> Primary
   ^           |          |
   +-----------+----------+
任意活动角色 -> Failed / Stopped
```

- `kStandby`：可 tail，不可 submit。
- `kPromoting`：已开始获取 writer 或执行 catch-up/barrier，不可 submit。
- `kPrimary`：barrier 已 durable+applied，可以 submit。
- `kFailed`：不可恢复的本地 apply/data-loss 错误。
- `kStopped`：生命周期结束。

`role_state.h` 中的 `RoleStateMachine` 是未来完整控制面状态机，使用独立的 `ReplicaRole` 类型；当前 `ReplicaRuntime` 尚未接入它。

### 7.3 `poll()`

Runtime 不创建隐藏线程。调用方必须周期调用 `poll()`：

- Standby：从 `AppliedLRSN + 1` tail 并 replay 全部当前 durable records；
- Primary：先续租当前 writer，再执行相同的 catch-up 检查；
- renew 失败或 WriterEpoch 改变：立即释放 writer、清空 epoch、回到 Standby。

`poll()` 周期必须显著小于 `writer_lease_timeout_ms`。调度暂停超过 lease timeout 时，LocalFS writer 不允许通过 renew 复活。

### 7.4 `promote_session()`、token 与角色回调

仅 Standby 可提升：

1. 进入 `kPromoting`；
2. `SharedWal::acquire_writer()` 获取更高 WriterEpoch；
3. 在旧 writer 已被 fence 后 catch-up 到当前 durable tail；
4. append + sync `kPrimaryBarrier`；
5. 本地 apply barrier，只推进 AppliedLRSN；
6. 进入 `kPrimary`。

acquire 失败、临时 catch-up 失败或 barrier append 失败均不会开放 admission。临时 catch-up 失败会释放 writer并回到 Standby，可再次调用 `promote_session()`。

成功提升返回 move-only RAII `PrimarySession`。其 `PrimaryToken` 同时绑定 runtime generation 与 WriterEpoch；所有 submit/checkpoint/demote 都校验 token，旧 session、旧任期或已 demote token 会被拒绝。session 通过 weak lifetime control block 访问 runtime，析构自动 demote；session 可以安全晚于栈上 runtime 析构，而不要求 runtime 由 `shared_ptr` 构造。`RoleLifecycle` 接收每次角色变化；回调在 runtime mutex 释放后按转换顺序由单一 dispatcher 串行执行，因此可以安全查询或重入 runtime，重入产生的新转换只入队并由当前 dispatcher 继续派发。

### 7.5 `submit()`

仅 `kPrimary` 可调用：

1. append mutation 到 WAL；
2. sync 并验证 durability proof；
3. 以 `ApplySource::kPrimaryCommit` 调用业务 `apply()`；
4. 返回包含 LRSN 与 applied/rejected 业务结果的 `CommitResult`。

`request_id` 应由业务提供稳定且非复用的值。当前框架将其写入 WAL，但尚不执行 durable dedupe。

### 7.6 `checkpoint()`

当前 checkpoint 是一个简化的 inline WAL record（record replay，不是 bootstrap checkpoint）：

1. Primary 在当前 AppliedLRSN 状态调用 `create_checkpoint()`；
2. 将快照字节写入 `kCheckpoint` record；
3. record durable 后调用本地 `restore_checkpoint()`；
4. Standby tail 到该 record 时执行相同 restore；
5. checkpoint record 自身的 LRSN成为新的 AppliedLRSN。

快照实际覆盖 checkpoint record 之前的业务状态，而传给 `restore_checkpoint()` 的 LRSN 是 checkpoint record 的 LRSN。当前接口尚未分离 `covered_applied_lrsn` 和 `checkpoint_record_lrsn`，不能据此实施 WAL GC。

新副本当前仍从 LRSN 1 扫描。构造 Runtime 时业务状态机必须是空状态；不能把已保留的非空状态与默认 AppliedLRSN 0 组合启动。

### 7.7 RAII demote、`ReadGuard` 与 `stop()`

`PrimarySession::demote()` 释放 writer、清空 WriterEpoch，并回到 Standby；session 析构执行同样操作且不会在持有 runtime mutex 时递归加锁。`kFailed` 不会被 demote 覆盖。`stop()` 幂等释放 writer并进入 `kStopped`。

`ReplicaRuntime::read(token)` 返回持有 runtime mutex 的 `ReadGuard`，与 apply 严格串行。guard 暴露一致的 `ReplicaStatus`、token validity 和状态机引用，业务只可在 guard 生命周期内读取状态机，且不得从 guard 内重入 runtime。

## 8. 业务状态机 SPI

```cpp
class ReplicatedStateMachine {
public:
virtual absl::StatusOr<ApplyResult> apply(
const LogRecord& record,
const ApplyContext& context) = 0;

    virtual absl::StatusOr<std::vector<std::byte>> create_checkpoint();
    virtual absl::Status restore_checkpoint(
        std::span<const std::byte> payload,
        Lrsn checkpoint_record_lrsn);
};
```

### 8.1 apply 契约

- 输入 record 已经 durable、CRC 正确且 LRSN 连续。
- Primary 和 Standby 对 replicated state 的转换必须完全相同。
- 必须先完成所有解析、范围和 schema 校验，再原子更新内存状态。
- 不得依赖 wall clock、随机数、地址、线程调度或不受控外部状态决定 replicated result。
- `ApplyResult::Applied()` 表示业务状态转换成功。
- `ApplyResult::Rejected(reason)` 表示确定性的业务拒绝；runtime 仍推进 AppliedLRSN，并通过 `CommitResult` 返回拒绝原因。
- `StatusOr` 的非 OK status 表示系统错误，runtime 进入 `kFailed`，该 record 不会被跳过。
- apply/restore 在 runtime mutex 内与 `ReadGuard` 串行，禁止重入 runtime；角色生命周期回调则在锁外执行。

### 8.2 ApplyContext

```cpp
enum class ApplySource : uint8_t {
    kPrimaryCommit,
    kWalReplay,
};

struct ApplyContext {
    ApplySource source;
    WriterEpoch writer_epoch;
    bool is_primary_commit() const;
};
```

- `kPrimaryCommit`：当前 Primary 刚使该 mutation durable，正在 ACK 前本地 apply。
- `kWalReplay`：Standby、重启副本或 promotion catch-up 从 WAL 读取该 mutation。

业务可以在 `kPrimaryCommit` 分支做本地环境准备，但该行为必须：

1. 不改变 replicated result；
2. 幂等、可重试；
3. 不承担 fencing；
4. 不把其他副本无法重放的外部结果写入内存状态。

如果环境准备是业务提交成功的必要条件，应设计成事务性/幂等 outbox 或独立的 Primary lifecycle，而不是在已修改状态后返回失败。

## 9. 存储后端

### 9.1 独立接口与能力位

`ObjectMetadataBackend` 只包含 `kImmutableObjects`。`ActiveLogStorage` 独立包含 `kDurableAppend`、`kWriterFencing`、`kDurableTail` 与 `kLeaseRecovery`。实现对象存储绝不隐含它能承载 authoritative WAL；`validate_authoritative_active_log()` 要求 active-log 四项能力全部具备。Election/coordinator 是第三个独立外部服务，不属于任一存储接口。

### 9.2 LocalFileSystem

LocalFS 实现：

- immutable object；
- active WAL；
- `flock` 串行 writer；
- sidecar `.minivessel.meta` 持久化 WriterEpoch、AssignmentEpoch、durable offset、lease、packet sequence 和 CRC；
- WAL fsync 后再原子发布 sidecar；
- recovery 时截断到 published durable offset；
- durable-prefix 限界读取；
- sealed WAL 拒绝重新打开。

LocalFS **不声明 `kLeaseRecovery`**。POSIX advisory lock 无法 fence 一个仍存活但长时间暂停的旧进程，所以它仅用于开发、单元测试和单机多进程演练，不是生产 shared WAL。

### 9.3 MiniDfsFileSystem

当前 `MiniDfsFileSystem` 只继承 `ObjectMetadataBackend`，仅提供 immutable checkpoint 与 sealed segment。它既不包含 active-log 方法，也不提供 MiniVessel election/metadata-lease RPC。它不应作为 MiniVessel active log 或 coordinator 嵌入。生产 HBase/Vessel 架构必须分别接入独立的 authoritative shared log 与外部 election/coordinator。若未来另行实现基于 MiniDFS 的 active log，则必须独立实现：

1. 存储端单调 WriterEpoch；
2. NameNode/DataNode 对每次 append/sync/seal 校验 epoch；
3. authoritative durable boundary；
4. durable active tail；
5. 不依赖旧主退出的 lease recovery；
6. crash 后完整 frame 边界恢复；
7. NameNode/metadata store 自身的 HA 和不回退 epoch allocator。

外部 election 只决定谁可以尝试 `promote_session()`，不等于数据面 fencing。没有 authoritative log 的 WriterEpoch 验证时，旧主暂停后恢复仍可能产生副作用。

## 10. Counter 接入示例

完整示例位于 `cpp/pl/minivessel/example/counter.cpp`。

### 10.1 状态机实现

Counter mutation payload 是 little-endian `int64_t delta`：

- `apply()` 先解析并检查有符号溢出；
- Primary 和 Standby 都执行相同加法；
- 只有 `kPrimaryCommit` 增加本地 preparation 计数；
- 溢出通过 `ApplyResult::Rejected` 形成确定性的业务拒绝，拒绝记录仍推进 AppliedLRSN；
- checkpoint payload 是当前 `int64_t value`；
- restore 校验 payload 后替换内存值；
- `CounterRoleLifecycle` 感知全部角色切换，并以 Primary-only resource 标志演示业务服务启停。

### 10.2 双副本与重启演练

示例覆盖：

1. A 的 lifecycle 观察 `Standby → Promoting → Primary` 并启动 Primary-only resource；
2. A 写入两条 mutation，B 故意滞后；
3. A 提交一条确定性 overflow rejection，该记录 durable 并推进 LRSN，但不修改 Counter；
4. B 作为 Standby 拒绝直接写入，然后 `poll()` 重放成功与拒绝记录并追平；
5. A 发布 checkpoint，B replay checkpoint；
6. A 再写一条，B 保持滞后；
7. A demote，lifecycle 停止 Primary-only resource，旧 `PrimaryToken` 被拒绝；
8. B promote 时先追平，再写 barrier、启动 Primary-only resource 并接管；
9. 构造全新 C，从 LRSN 1 replay mutation、rejection、barrier 和 checkpoint；
10. B demote，C promote 并写入，A/B/C 最终收敛；
11. `ReadGuard` 校验 C 的 token 并一致读取业务状态；
12. 丢弃 C 的 RAII `PrimarySession` 自动 demote，随后 stop 三个副本并校验完整角色序列；
13. replay 副本不执行 Primary-only preparation。

### 10.3 构建和运行

```bash
bazel build //cpp/pl/minivessel/example:counter
bazel run //cpp/pl/minivessel/example:counter
```

核心测试：

```bash
bazel test //cpp/pl/minivessel/... --test_output=errors
```

MiniDFS Docker E2E：

```bash
cd docker/minidfs
cp -n .env.example .env
chmod 600 .env
./tests/e2e.sh build
./tests/e2e.sh start
./tests/e2e.sh minivessel-test
```

已有集群上只重跑测试时直接执行 `./tests/e2e.sh minivessel-test`；`start` 会重建测试环境。

## 11. 最小接入流程

```cpp
LocalFileSystem filesystem;
FramedSharedWal wal(&filesystem, {
    .group = {.group_id = "counter", .incarnation = GroupIncarnation(1)},
    .path = "/tmp/counter/active.wal",
});
CounterStateMachine state_machine;
CounterRoleLifecycle lifecycle("replica-a");
ReplicaRuntime replica(&wal, &state_machine, {
    .replica_id = "replica-a",
    .assignment_epoch = AssignmentEpoch(1),
    .role_lifecycle = &lifecycle,
});
lifecycle.attach(&replica);

// 外部选主确认该副本应接管后：
auto session = replica.promote_session();
if (!session.ok()) {
    // 保持 Standby，稍后重试或继续 poll。
}

// 调用方的定时调度器必须持续驱动：
auto status = replica.poll();

// 仅凭当前 session/token 提交；payload 编码由业务定义：
auto committed = session->submit("stable-request-id", payload);

// 在 guard 生命周期内一致读取业务状态：
auto guard = replica.read(session->token());

// 优雅让主（忘记显式调用时 session 析构也会 demote）：
status = session->demote();
replica.stop();
```

接入检查项：

- 每个副本指向同一个逻辑 WAL lineage；
- `replica_id` 稳定且非空；
- AssignmentEpoch 单调；
- 状态机初始为空；
- `poll()` 周期小于 writer lease timeout；
- mutation 确定且可重放；
- 请求重试前由业务提供幂等保护；
- apply callback 与 `ReadGuard` 内不重入 runtime；`RoleLifecycle` 可在锁外查询 runtime；
- 生产启动前独立验证 object backend 与 authoritative active-log capabilities。

## 12. 错误语义

| 场景 | 返回/状态 |
|---|---|
| Standby submit | `FailedPrecondition`，角色不变 |
| writer acquire 冲突 | promote 失败，回到 Standby |
| promotion 临时 tail 失败 | 释放 writer，回到 Standby，可重试 |
| barrier append/sync 失败 | 释放 writer，回到 Standby，结果 uncertain |
| mutation append/sync 失败 | 立即关闭 admission，回到 Standby，结果 uncertain |
| checkpoint append/sync 失败 | 立即关闭 admission，回到 Standby，结果 uncertain |
| record gap/duplicate/CRC 错误 | `kFailed` |
| deterministic 业务拒绝 | `CommitResult` 为 rejected，推进 AppliedLRSN，角色不变 |
| apply/restore 系统错误 | `kFailed` |
| Primary poll 续租失败 | 释放 writer，回到 Standby |
| stop 后 poll/submit | `FailedPrecondition` |

`Unavailable` 只在 promotion catch-up 中视为可重试；corruption、LRSN 不连续和 apply 失败均不可静默跳过。

## 13. 审查结论与生产 No-Go

本轮审查已修复：

- 全代码库统一 `Lrsn` / `lrsn` 术语；
- sync durability proof 的 WriterEpoch 和完整 frame 边界校验；
- promotion catch-up 临时失败卡住 writer；
- barrier 失败残留 WriterEpoch；
- writer owner 在 renew 后丢失；
- LocalFS 过期 writer 可续租复活；
- LRSN/offset/packet sequence 溢出保护；
- Runtime 简化角色与完整角色枚举同名冲突；
- Counter 有符号溢出的未定义行为。

以下任一项未完成前，MiniVessel 仍是生产 No-Go：

1. durable request dedupe 与 uncertain outcome 查询；
2. checkpoint manifest、coverage LRSN、外置对象校验和快速 bootstrap；
3. WAL segmentation、retention 与安全 GC；
4. Runtime 使用 monotonic admission deadline 自动续租/停写；
5. 有界异步执行器、背压和业务 callback 隔离；
6. 独立 authoritative active WAL、存储端 fencing 和 lease recovery；
7. 外部 election/coordinator 集成及其与 `PrimarySession` 生命周期的绑定；
8. clock skew、RPC 响应丢失、crash point 和 stale-primary 故障矩阵；
9. decoder fuzz、golden bytes、TSan/ASan 与多进程 kill 演练；
10. 指标、审计、inspect 和恢复工具。

在这些能力完成前，core `FramedSharedWal` 只用于协议与业务接入验证；MiniDFS adapter 对通用 active WAL 仍须 fail-closed。`e2e/` 的独立 shared-log authority 是范围受限的测试实现，不能据此解除生产 No-Go。

## 14. 存储系统中的对象 layout（E2E 已实现）

这里的 layout 是 **MiniVessel 写入 MiniDFS 的对象路径布局**，不是源码目录。当前 E2E 是 object-per-record：

```text
/minivessel/groups/<escaped-group>/<incarnation>/wal/records/<20位零填充LRSN>-e<WriterEpoch>-<object-id>.record
/minivessel/groups/<escaped-group>/<incarnation>/wal/checkpoints/<20位零填充LRSN>-e<WriterEpoch>-<object-id>.checkpoint
```

例如：

```text
/minivessel/groups/counter/1/wal/records/00000000000000000003-e1-a942....record
/minivessel/groups/counter/1/wal/checkpoints/00000000000000000004-e1-1465....checkpoint
```

### 14.1 稳定编码与 traversal 防护

`escaped-group` 按字节编码：ASCII 字母数字、`-`、`_`、`.` 原样保留，其余字节用大写 `%HH`；空 segment、`.`、`..` 被拒绝。这样 `/`、反斜线、空白和非 ASCII 字节都不能形成路径分隔。incarnation、LRSN、WriterEpoch 来自强类型无符号整数。object-id 是 request ID 的稳定 FNV-1a 摘要；它用于避免可读路径携带任意输入，不承担密码学完整性。路径格式是稳定的 E2E 内部契约。

### 14.2 创建、内容、身份和发布

| 对象 | 谁/何时创建 | binary content | 生命周期 |
|---|---|---|---|
| `.record` | shared-log 接受有效 writer 的 mutation/barrier 后 | protobuf：LRSN、WriterEpoch、类型、request ID、payload | immutable；close 成功并取得 `FileIdentity` 后才发布 catalog |
| `.checkpoint` | Primary 调 `checkpoint()`，shared-log 接收 checkpoint record 后 | 同一 record protobuf，payload 是状态机 checkpoint bytes | immutable；同样先 close 再发布 |

`FileIdentity` 包含 inode、content generation、length 和 checksum。authority catalog 保存连续 LRSN 到 `(path, FileIdentity)` 的映射。read 不信任目录，也不只信任 path：它按 catalog 的 identity-bound read 从 MiniDFS 读回 binary，再校验解码出的 LRSN/epoch。payload 不进入 MySQL 或 Vessel 服务文件；MySQL 仅承载 MiniDFS 自己的 namespace/block metadata。

### 14.3 recovery、orphan 与 GC

replica 重启从 LRSN 1 调 `Read`；authority 按 catalog 顺序从 MiniDFS 对象读取，`ReplicaRuntime` 连续 replay，其中 checkpoint record 会恢复状态，然后继续应用后续 record。当前 E2E 没做 checkpoint 跳跃优化。

如果对象 close 成功但进程在 catalog insert 前崩溃，该对象是 orphan；如果 insert 已发生，则它是 published。**禁止用 MiniDFS `list` 重建或裁决 authority**，因为 listing 可能包含 orphan，且文件名不是提交证明。E2E authority crash 后 catalog 不恢复，GC 也未实现；生产实现必须以持久化 catalog 做 mark-and-sweep，按 `FileIdentity` 条件删除未引用对象并设置安全期。segment/manifest 路径尚未实际使用，只是未来能力，不属于当前 layout。

### 14.4 与 core 单文件布局的区别

core `FramedSharedWal` 把 frame（header/payload/CRC）追加到一个 active 文件，checkpoint payload inline 于 checkpoint frame。E2E `RemoteSharedWal` 的 wire 仍传完整 `LogRecord` 和 bytes，但 authority 将**每个逻辑 record 整体**序列化为独立 immutable MiniDFS object；checkpoint record 因而也是外置 object。二者是两个 backend layout，不应混用格式或 recovery 假设。

## 15. 对外接口功能与何时使用

| 接口 | 调用者与时机 | 线程/错误语义 | 禁用法 |
|---|---|---|---|
| `ReplicatedStateMachine::apply` | Runtime 按连续 mutation 调用 | runtime 锁内；非 OK 是系统失败，`ApplyResult::Rejected` 是确定性结果且推进 LRSN | 不做非确定状态转移，不重入 Runtime |
| `ApplyResult` / `ApplyContext` | 业务区分 applied/rejected 与 primary-commit/replay | rejection 必须可重放；context 仅允许控制本地幂等准备 | 不把业务拒绝伪装成系统错误，不让 primary-only 动作改变复制结果 |
| `RoleLifecycle` | Runtime 在角色变化后通知业务启停 Primary-only 资源 | **锁外、有序、串行**回调，可查询或重入 Runtime；快照可能已落后于最新角色，但后续转换不会乱序 | 不在回调中假设角色仍未变化，不泄漏 Primary 资源 |
| `ReplicaRuntime::poll` | 服务后台线程周期调用，Standby tail、Primary renew+tail | 内部串行；临时 read unavailable 可返回，数据损坏转 Failed | poll 周期不得超过 lease，不并发驱动同一业务对象的危险逻辑 |
| `promote_session` | 外部选主后由服务调用并持有返回值 | acquire/fence 旧 writer → catch-up durable tail → barrier → Primary；失败释放 writer | 不丢弃临时 session，不绕过外部选主 |
| `status` / `read` | 管理面查询、业务一致读取 | `status` 是快照；`ReadGuard` 持 runtime 锁并可验证 token | 持 `ReadGuard` 时不回调 Runtime，不直接无锁读取业务值 |
| `stop` | 服务停止接入和退出前 | 幂等释放 writer、进入 Stopped | stop 后不可 poll/submit |
| `PrimarySession` / `PrimaryToken` | 服务在 Primary 全生命周期持有 session；session 提交/checkpoint/demote | RAII 析构 fencing；token 同时绑定 generation+WriterEpoch | 不缓存旧 token 写入，不复制 session，不让局部变量立即析构 |
| `ReadGuard` | status/read RPC 读取 state machine | 锁保护状态与角色快照 | 不把 state machine 引用带出 guard 生命周期 |
| `SharedWal` | 每个 replica 装配一个实例；Runtime 隐式使用该实例当前 writer session | acquire/renew/release/append/read/durable；实现必须并发安全或由上层串行 | 不在多个 replica 间共享一个有隐式 session 的 client 对象 |
| `ActiveLogStorage` | `FramedSharedWal` backend 实现 active file fencing/sync | sync proof 必须绑定 epoch 和 durable boundary | object backend 不应假装支持 appendable active WAL |
| `ObjectMetadataBackend` | 对象型 checkpoint/数据组件保存 identity/metadata（生产扩展点） | metadata 必须在 object durable 后发布 | 不以目录 listing 代替 metadata authority，不先发布后持久化 |

E2E 管理面提供 `status/promote/demote/add/checkpoint/poll/stop`；正常服务使用后台 poll，显式 `poll` 仅用于诊断和确定性测试。`status` 输出 role、value、AppliedLRSN、WriterEpoch、Primary-only resource 状态和生命周期计数。