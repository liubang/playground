# MiniDFS — C++20 分布式文件系统

MiniDFS 是一个用 C++20 实现的 HDFS-like 分布式文件系统，基于 brpc 进行 RPC 通信，使用 MySQL 存储元数据。项目追求极致性能（硬件加速 CRC32C 校验、零拷贝 I/O、连接池复用）和工程品质（强类型抽象、RAII 资源管理、编译期约束）。

## 架构

```text
                  +----------------------+
                  |        Client        |
                  | CLI / SDK / Library  |
                  +----------+-----------+
                             |
                             | brpc (Metadata RPC)
                             |
                  +----------v-----------+
                  |       NameNode       |
                  |----------------------|
                  | NamespaceManager     |
                  | BlockManager         |
                  | DataNodeManager      |
                  | PlacementManager     |
                  | ReplicationManager   |
                  | LeaseManager         |
                  | AdminService         |
                  +----------+-----------+
                             |
                  +----------v-----------+
                  |  MySQL MetadataStore |
                  +----------------------+

  +----------------+    +----------------+    +----------------+
  |   DataNode 1   |    |   DataNode 2   |    |   DataNode 3   |
  |----------------|    |----------------|    |----------------|
  | LocalBlockStore|    | LocalBlockStore|    | LocalBlockStore|
  | PipelineRecv   |    | PipelineRecv   |    | PipelineRecv   |
  | BlockReporter  |    | BlockReporter  |    | BlockReporter  |
  | HeartbeatSender|    | HeartbeatSender|    | HeartbeatSender|
  | ReplicationWkr |    | ReplicationWkr |    | ReplicationWkr |
  +----------------+    +----------------+    +----------------+
```

## 技术栈

| 领域     | 选型                     | 说明                           |
| -------- | ------------------------ | ------------------------------ |
| 语言     | C++20                    | concepts, ranges, constexpr    |
| 构建     | Bazel 8 (bzlmod)         | 确定性构建                     |
| RPC      | brpc + protobuf          | 高性能、低延迟                 |
| 元数据   | MySQL (Boost.MySQL)      | 异步连接池、类型安全           |
| 校验     | CRC32C                   | Linux: ISA-L (SIMD)，macOS: crc32c (ARM HW) |
| 压缩     | zstd / snappy            | 可选块压缩                     |
| 错误处理 | pl::Result (folly::Expected) | 类型安全的错误传播         |
| 日志     | folly xlog               | 结构化日志                     |

## 目录结构

```
cpp/pl/minidfs/
├── common/          # 公共类型、常量、校验、压缩、错误码
│   └── tests/       # common 单元测试（5 个 target）
├── protocol/        # protobuf 服务定义
├── metadata/        # MetadataStore 接口及 MySQL 实现
├── namenode/        # NameNode 业务逻辑（6 个 Manager）
│   └── tests/       # namenode 单元测试（6 个 target）
├── datanode/        # DataNode 存储引擎及服务
│   └── tests/       # datanode 单元测试（5 个 target）
├── master/          # NameNode 启动入口
├── client/          # CLI 工具及 DfsClient SDK
├── docker-compose.yml
├── Dockerfile
├── spec.md          # 详细设计规约
└── readme.md
```

## 构建依赖

### 系统工具

- Bazel 8+（推荐通过 [bazelisk](https://github.com/bazelbuild/bazelisk) 安装）
- C++20 编译器：Clang 16+ 或 GCC 13+
- autoconf, automake, libtool（ISA-L 构建需要，仅 Linux）
- nasm（ISA-L 汇编优化需要，仅 Linux x86_64）
- pkg-config

macOS 安装：

```bash
brew install bazelisk pkg-config
```

Ubuntu/Debian 安装：

```bash
sudo apt-get install -y build-essential clang lld git curl \
    autoconf automake libtool nasm pkg-config
# 安装 bazelisk
curl -fSL https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64 \
    -o /usr/local/bin/bazel && sudo chmod +x /usr/local/bin/bazel
```

### 第三方库（Bazel 自动管理）

以下依赖由 `MODULE.bazel` 声明，Bazel 会自动下载，无需手动安装：

- brpc 1.16.0（RPC 框架）
- protobuf 31.1（序列化）
- folly 2025.01.13（Expected、xlog）
- Boost.MySQL 1.90（异步 MySQL 客户端）
- ISA-L 2.31（硬件加速 CRC32C，仅 Linux）
- crc32c（硬件加速 CRC32C，仅 macOS）
- zstd 1.5.6、snappy 1.2.1（压缩）
- fmt 12.1.0（格式化）
- gflags 2.2.2（命令行参数）
- googletest 1.17（单元测试）

### 外部服务

- MySQL 8.0+（NameNode 元数据存储）

## 构建

```bash
# 构建全部
bazel build //cpp/pl/minidfs/...

# 单独构建各组件
bazel build //cpp/pl/minidfs/master:namenode       # NameNode 服务
bazel build //cpp/pl/minidfs/client:datanode       # DataNode 服务
bazel build //cpp/pl/minidfs/client:minidfs        # CLI 客户端

# 优化构建（推荐用于部署）
bazel build -c opt //cpp/pl/minidfs/master:namenode
bazel build -c opt //cpp/pl/minidfs/client:datanode
bazel build -c opt //cpp/pl/minidfs/client:minidfs
```

构建产物位于：

```
bazel-bin/cpp/pl/minidfs/master/namenode      # NameNode 二进制
bazel-bin/cpp/pl/minidfs/client/datanode      # DataNode 二进制
bazel-bin/cpp/pl/minidfs/client/minidfs       # CLI 二进制
```

## 运行测试

```bash
# 全部单元测试（16 个 test target）
bazel test //cpp/pl/minidfs/...

# 按模块运行
bazel test //cpp/pl/minidfs/common/tests/...     # common: 5 个
bazel test //cpp/pl/minidfs/namenode/tests/...   # namenode: 6 个
bazel test //cpp/pl/minidfs/datanode/tests/...   # datanode: 5 个
```

## 手动部署

### 1. 初始化数据库 (format)

使用 format 工具自动创建数据库和表结构：

```bash
./format --schema_file=cpp/pl/minidfs/metadata/schema.sql \
    --mysql_host=127.0.0.1 --mysql_port=3306 --mysql_user=root --mysql_password=<pwd> \
    [--mysql_database=minidfs] [--force]
```

`--force` 会先删除已有数据库再重建，适用于开发环境重置。`--mysql_database` 默认为 `minidfs`。

也可以手动执行 schema.sql：

```bash
mysql -h <host> -u root -p < cpp/pl/minidfs/metadata/schema.sql
```

确保创建好用于连接的数据库用户并授权：

```sql
CREATE USER 'minidfs'@'%' IDENTIFIED BY '<your_password>';
GRANT ALL PRIVILEGES ON minidfs.* TO 'minidfs'@'%';
FLUSH PRIVILEGES;
```

### 2. 启动 NameNode

```bash
./namenode \
    -port=9000 \
    -mysql_host=127.0.0.1 \
    -mysql_port=3306 \
    -mysql_user=minidfs \
    -mysql_password=<your_password> \
    -mysql_database=minidfs \
    -mysql_pool_size=8
```

### 3. 启动 DataNode（每台机器一个）

```bash
./datanode \
    -port=9100 \
    -storage_root=/data/minidfs/dn1 \
    -namenode_addr=<namenode_host>:9000 \
    -hostname=$(hostname) \
    -ip=<本机IP> \
    -rpc_port=9100 \
    -rack=/rack1 \
    -heartbeat_interval_ms=3000 \
    -block_report_interval_ms=600000
```

不同 DataNode 使用不同的 `storage_root` 和端口（如果部署在同一台机器上）。

### 4. 使用 CLI 客户端

```bash
./minidfs -namenode=<namenode_host>:9000 <command> [args...]
```

## Docker 部署

使用 docker compose 一键启动完整集群（1 NameNode + 3 DataNode + MySQL）：

```bash
cd cpp/pl/minidfs

# 创建 .env 文件配置密码（可选，有默认值）
cat > .env <<EOF
MYSQL_USER=minidfs
MYSQL_PASSWORD=your_password
MYSQL_ROOT_PASSWORD=your_root_password
EOF

# 启动集群
docker compose up -d

# 查看状态
docker compose ps

# 查看日志
docker compose logs -f namenode
docker compose logs -f datanode1

# 销毁环境（含数据卷）
docker compose down -v
```

端口映射：

| 服务      | 容器端口 | 宿主端口 |
| --------- | -------- | -------- |
| MySQL     | 3306     | 13306    |
| NameNode  | 8000     | 18000    |
| DataNode1 | 9000/9001| 19000/19001 |
| DataNode2 | 9000/9001| 19010/19011 |
| DataNode3 | 9000/9001| 19020/19021 |

## 支持的操作

### 文件系统操作

| 操作   | 说明                             |
| ------ | -------------------------------- |
| mkdir  | 创建目录（支持多级路径）         |
| ls     | 列出目录内容（-d/-h/-R/-t/-S/-r）|
| stat   | 查看文件/目录详细信息            |
| put    | 上传本地文件到 DFS               |
| get    | 从 DFS 下载文件到本地            |
| rm     | 删除文件或目录（-r 递归删除）    |
| mv     | 重命名/移动文件或目录            |

### 管理与诊断操作

| 操作       | 说明                                       |
| ---------- | ------------------------------------------ |
| fsinfo     | 查看集群概览（容量、节点数、块数、文件数） |
| datanodes  | 列出所有 DataNode 及状态                   |
| datanode   | 查看单个 DataNode 详细信息                 |
| inode      | 查看 inode 详情（支持 ID 或路径查询）      |
| blocks     | 查看文件的所有 block 及副本分布            |
| block      | 查看单个 block 的副本详情                  |

## CLI 命令参考

```
Usage: minidfs [options] <command> [args...]

File System Commands:
  mkdir <path>               创建目录
  ls [-d] [-h] [-R] [-t] [-S] [-r] [path]
                             列出目录内容（默认 /）
      -d  显示目录自身信息，不列出内容
      -h  以 human-readable 格式显示大小
      -R  递归列出所有子目录
      -t  按修改时间排序（最新在前）
      -S  按文件大小排序（最大在前）
      -r  反转排序顺序
  stat <path>                查看文件/目录详细信息
  rm [-r] <path>             删除文件或目录（-r 递归）
  mv <src> <dst>             重命名/移动
  put <local> <dfs_path>     上传本地文件
  get <dfs_path> <local>     下载文件到本地

Admin Commands:
  fsinfo                     查看集群概览
  datanodes [-a|--all]       列出所有 DataNode（-a 含已下线节点）
  datanode <id>              查看 DataNode 详情
  inode <id|path>            查看 inode 详情
  blocks <id|path>           列出文件的 block 列表
  block <block_id>           查看 block 详情及副本

Options:
  -namenode=<host:port>      NameNode 地址（默认 127.0.0.1:8020）
  -rpc_timeout_ms=<ms>       RPC 超时时间（默认 5000）
  -replication=<n>           副本数（默认 3）
  -block_size=<bytes>        块大小（默认 128MB）
```

### 使用示例

```bash
# 连接 NameNode
export NAMENODE=192.168.1.100:9000

# 创建目录
minidfs -namenode=$NAMENODE mkdir /data
minidfs -namenode=$NAMENODE mkdir /data/logs

# 上传文件
minidfs -namenode=$NAMENODE put ./access.log /data/logs/access.log

# 查看目录
minidfs -namenode=$NAMENODE ls /data/logs

# 递归列出目录（human-readable 大小）
minidfs -namenode=$NAMENODE ls -hR /data

# 按修改时间排序
minidfs -namenode=$NAMENODE ls -t /data/logs

# 按文件大小倒序排列
minidfs -namenode=$NAMENODE ls -Sr /data/logs

# 只看目录自身属性
minidfs -namenode=$NAMENODE ls -d /data/logs

# 查看文件详情
minidfs -namenode=$NAMENODE stat /data/logs/access.log

# 下载文件
minidfs -namenode=$NAMENODE get /data/logs/access.log ./downloaded.log

# 移动/重命名
minidfs -namenode=$NAMENODE mv /data/logs/access.log /data/logs/access.log.bak

# 删除文件
minidfs -namenode=$NAMENODE rm /data/logs/access.log.bak

# 递归删除目录
minidfs -namenode=$NAMENODE rm -r /data/logs

# 指定副本数和块大小上传
minidfs -namenode=$NAMENODE -replication=2 -block_size=67108864 put ./large.dat /data/large.dat
```

### 管理命令示例

```bash
# 查看集群概览
minidfs -namenode=$NAMENODE fsinfo

# 列出所有活跃 DataNode
minidfs -namenode=$NAMENODE datanodes

# 列出所有 DataNode（含已宕机）
minidfs -namenode=$NAMENODE datanodes --all

# 查看某个 DataNode 详情
minidfs -namenode=$NAMENODE datanode 1

# 通过路径查看 inode 信息
minidfs -namenode=$NAMENODE inode /data/logs/access.log

# 通过 inode ID 查看
minidfs -namenode=$NAMENODE inode 42

# 查看文件的 block 分布
minidfs -namenode=$NAMENODE blocks /data/logs/access.log

# 查看单个 block 的副本详情
minidfs -namenode=$NAMENODE block 1001
```

## 设计要点

**Block 存储**：文件被拆分为固定大小的 Block（默认 128MB），每个 Block 维护 3 副本分布在不同 DataNode 上。Block 使用自描述二进制格式（BlockHeader），支持分 chunk 校验和可选压缩。

**写入流水线**：Client 写入时，数据通过 pipeline 依次转发到多个 DataNode，确保所有副本写入完成后才返回成功。

**心跳与副本管理**：DataNode 定期向 NameNode 发送心跳汇报状态，NameNode 据此维护集群视图、检测宕机节点并触发副本补充。

**租约机制**：文件写入通过 Lease 保证互斥，避免并发写入冲突。

**CRC32C 校验**：Linux 使用 Intel ISA-L 库的 SIMD 加速实现，macOS 使用 Google crc32c 库（利用 ARM 硬件 CRC 指令）。两者计算结果一致（相同多项式），每个 chunk 独立校验，支持增量计算和快速验证。

更多设计细节参见 [spec.md](spec.md)。
