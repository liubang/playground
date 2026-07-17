# Changelog

## 2026-07-17

### 不可变文件发布与身份

- 新增 `FileAppendMode` 和稳定的 `FileIdentity(inode_id, content_generation, length, checksum)`；immutable 文件 Complete 后拒绝 append、truncate 和原地覆盖。
- Complete 必须提交客户端计算的最终长度与内容 CRC32C，并原子返回已发布 identity；幂等 replay 必须与已发布 identity 一致且不重复增加 generation。
- appendable 文件 truncate 后增加 content generation，并将未知的内容 checksum 明确标记为无效，不再以块元数据伪装文件内容校验和。

### 身份绑定随机读

- `DfsClient::read_exact` 支持绑定调用方提供的 `FileIdentity`，按已发布长度跨 Block 精确读取，并对超时、短读、校验失败执行副本回退。
- GetLocatedBlocks、token refresh 和 Block Token 全程绑定相同文件 identity，拒绝路径重用或 generation 变化产生的 stale read。
- DataNode 使用 `pread` 实现真正的范围读取，只读取相交 chunk，同时验证每个相交 chunk 的完整落盘 CRC32C。

### sstv2 集成

- 新增 sstv2 `MiniDfsFileSystem` 后端，通过统一 `FileSystem`/`FileHandle` 接口将 key/value sink 追加写入 immutable MiniDFS 文件，并在 `close` 时发布。
- `open` 固定已发布 identity，`read_at` 通过 `read_exact` 精确填充 sstv2 Reader 提供的字节缓冲区；后端共享持有 `DfsClient` 以覆盖打开句柄的生命周期。
- E2E 覆盖 embedded/separated value、point get、Seek/Next 全量迭代、跨 Block 小范围读取、immutable mutation 拒绝和 DataNode 重启恢复。

### 验证

- `bazel test //cpp/pl/minidfs/... //cpp/pl/sstv2/... --test_output=errors` 全部 34 个测试目标通过。
- `docker/minidfs/tests/e2e.sh all` 从空卷执行通过，包括 CLI、DfsClient/sstv2 集成、DataNode 重启和启动 block report。

## 2026-07-16

### NameNode 与元数据一致性

- 禁止将目录重命名到自身或子孙目录，防止命名空间形成环。
- 块提交要求满足最小写副本数，同时兼容单副本文件配置。
- 为文件逻辑块 `(inode_id, block_index)` 增加唯一约束，并通过事务及失败补偿避免并发重复分配和幽灵块。
- 递归删除遍历失败时立即终止并回滚，避免 inode 删除后遗留无法回收的块。

### RPC 幂等性

- 客户端为有副作用的 NameNode RPC 设置稳定且唯一的请求 ID。
- oplog 持久化操作响应，使重复的 `Mkdir`、`CreateFile` 和 `AllocateBlock` 请求能够重放完整业务结果。
- 将幂等记录写入纳入操作事务，避免业务操作成功但幂等状态缺失。

### DataNode 可靠性

- 块删除命令同时携带 block ID 和 generation stamp，并传播删除失败。
- heartbeat 和 block report 检查 NameNode 返回的业务状态。
- 块追加失败时恢复原始文件长度和 header，避免部分写污染后续重试。
- 块写入、finalize rename 及目录变更增加持久化同步，并兼容 macOS `F_FULLFSYNC`。
- DataNode 启动时清理超过阈值的陈旧临时块，并保护仍活跃的临时块。
- 使用 POSIX 文件时间计算临时块年龄，避免 filesystem clock 差异。

### 数据面安全

- 增加由 NameNode 签发的短期 HMAC-SHA256 Block Token。
- Token 绑定 block ID、generation stamp、inode ID、block index、权限与过期时间。
- 为 `WriteBlock`、`ReadBlock`、`TransferBlock` 和 `TruncateBlock` 增加权限校验。
- 将 Token 贯通客户端写入、读取和 DataNode 副本复制流程，并使用恒定时间签名比较。

### 测试

- 增加命名空间环、最小写副本、块分配失败补偿和递归删除回滚测试。
- 增加 generation stamp 删除、陈旧临时块清理和控制面错误测试。
- 增加 Block Token 签发、过期、篡改、权限隔离及各数据面 RPC 鉴权测试。
- `bazel test //cpp/pl/minidfs/... --test_output=errors` 全部 21 个测试目标通过。
