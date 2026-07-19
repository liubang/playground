# MiniVessel Counter 多进程 E2E 验收

## 拓扑与边界

测试启动 9 个独立容器：MySQL、MiniDFS NameNode、3 个 DataNode、独立 shared-log authority，以及 `replica-a/b/c` 三个 Counter 服务。Vessel RPC 与进程均不嵌入 MiniDFS；MySQL 仅保存 MiniDFS 自身元数据，Counter payload 只存在 MiniDFS immutable object 中。DataNode volume 是 MiniDFS 正常块存储，不是 LocalFS WAL。

shared-log 的 catalog、epoch、lease 是服务内存状态，因此 shared-log 崩溃恢复明确不在本 E2E 范围；replica 重启恢复在范围内。该实现用于验证对象持久化、**authority fencing**、远程 replay 与角色切换，不是生产 authority，也不声称 MiniDFS storage 自身提供 writer fencing。catalog 无持久化和容量治理，进程生命周期内会随 record 数量增长。

## 前提与命令

需要 Docker、Compose v2、可访问 `liubang/bazel-builder:8.7.0`，建议至少 6 GiB 可用内存。

```bash
docker/minivessel/tests/e2e.sh all    # 清空、构建、启动、验收、成功后停止
docker/minivessel/tests/e2e.sh build  # 只构建镜像
docker/minivessel/tests/e2e.sh start  # 启动并等待健康
docker/minivessel/tests/e2e.sh test   # 对已启动且洁净的集群执行验收；非洁净状态会 fail-closed
docker/minivessel/tests/e2e.sh down   # 停止，保留 MiniDFS 数据卷
docker/minivessel/tests/e2e.sh reset  # 删除容器和全部数据卷
```

失败时脚本保留现场并输出最近 300 行日志；成功的 `all` 会停止容器但保留 volume。`test` 开始前要求 shared-log durable LRSN/object count/epoch 和三个 replica 的状态均为初始值，避免旧数据产生假阳性；完整重跑请使用 `all`（它会先删除 volume）。

## 分阶段动作、预期与证据

1. **A 成为 Primary 并写入。** 外部 `minivessel-control` 调用 `promote` 和两次 `add`；B/C 后台 poll。预期三者值为 7、AppliedLRSN 相同。证据是每次提交返回 `lrsn=N` 和 `status` 的 shell 可解析字段。
2. **checkpoint record 与静态副本报告。** A 调用 `checkpoint`。它是参与正常日志回放的 inline record，不是跳过旧日志的 bootstrap checkpoint。脚本使用 MiniDFS CLI 列出 records/checkpoints，要求对象总数精确为 4、断言零填充 LRSN 文件名存在，并逐对象要求至少一个 block 且全部 block 的 NameNode 静态报告均为 `committed/desired=3/actual=3`；这是静态报告，不是持续可用性证明。目录 listing 只用于验收观察，不参与 authority 或 replay。
3. **真实陈旧 session fencing。** 保存 A epoch，并通过 RPC 暂停 A 的后台 polling，保持其 `PrimarySession`，不 demote A。等待 5 秒 lease 自然过期后，在 12 秒明确界限内重试 promote B；新 epoch 必须严格增大。
4. **authority 拒绝证据。** 在 A 陈旧写前后读取 shared-log status。A 的 `Add` 必须从 authority 精确返回 `ABORTED`，durable LRSN/object count 不变，随后 A runtime 因 append 失败转为 Standby。该步骤不调用 A demote 冒充 fencing。
5. **落后与重启。** B 写入 5，记录 C 的容器 PID，停止 C 并确认 `exited` 后让 B 写入 8，再使用 `start --wait` 启动 C，断言 PID 变化。C 作为新进程通过 RemoteSharedWal 读取 catalog 中的 `FileIdentity`，从 MiniDFS immutable objects replay 到目标值/LRSN。
6. **切换到 C。** demote B、promote C、C 写入 2，A/B/C 最终收敛到 value=22 和相同 AppliedLRSN。
7. **生命周期与资源。** 脚本按进程生命周期断言精确 transition 次数：A=3、B=3、重启后的 C=2；A/B 的 `resources=inactive`，C 为 `active`。C 重启前的计数不跨进程保留。最后还要求 authority 的 durable LRSN/object count 与最终提交 LRSN 相等，并确认全部 9 个容器仍为 running/healthy。

## MiniDFS 对象检查

实际路径：

```text
/minivessel/groups/counter/1/wal/records/00000000000000000001-e1-<object-id>.record
/minivessel/groups/counter/1/wal/checkpoints/00000000000000000004-e1-<object-id>.checkpoint
```

可手工检查：

```bash
docker compose -f docker/minivessel/docker-compose.yml exec namenode \
  minidfs --namenode=namenode:9000 ls /minivessel/groups/counter/1/wal/records
docker compose -f docker/minivessel/docker-compose.yml exec namenode \
  minidfs --namenode=namenode:9000 ls /minivessel/groups/counter/1/wal/checkpoints
```

每个 object 的 protobuf binary 包含 LRSN、WriterEpoch、类型、request ID 和 payload。shared-log 在 close 取得 `FileIdentity` 后才把它加入连续 catalog；read 使用 path + `FileIdentity` 绑定读取并再次解析校验。

## 故障排查

- `docker compose ... ps`：确认 3DN、NameNode、sharedlog 和 replica 均 healthy。
- `docker compose ... logs sharedlog replica-a replica-b replica-c`：检查 lease/fencing/RPC。
- `docker compose ... logs namenode datanode1 datanode2 datanode3`：检查 immutable close、block pipeline 与副本数。
- 首次构建资源不足时提高 Docker 内存；不要改成宿主 LocalFS WAL规避。
- 若失败后重跑完整场景，先执行 `reset`，否则对象同名创建会按 immutable 语义失败。

## 已知非生产边界

shared-log catalog、lease、epoch 不持久化且不是 HA，整个验收期间 authority 不重启；没有跨 authority crash 的 catalog recovery、orphan 扫描/GC、鉴权/TLS、限流和容量治理。Append 为保持该小规模 E2E 的线性化会在 authority mutex 内执行 MiniDFS I/O，并在 I/O 后重新校验 lease；若 lease 期间过期，已关闭但未发布的对象成为 documented orphan。路径与对象格式是 E2E 内部契约。未来生产实现应持久化 authority catalog，并以 catalog 为唯一权威；MiniDFS `ls` 永远不能替代 catalog。
