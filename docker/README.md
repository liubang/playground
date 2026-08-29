# Docker 实验环境

本地开发/测试用的容器化实验环境。每个子目录是一个**独立的实验模块**，各自维护 `docker-compose.yml`、网络和数据卷，互不依赖，按需单独启动。

> ⚠️ 仅适用于开发/测试环境，不可用于生产。HDFS 单副本、明文密码、无 TLS 等配置不适合生产部署。

## 模块一览

| 模块                    | 说明                                                      | 启动命令                       |
| ----------------------- | --------------------------------------------------------- | ------------------------------ |
| [`bigdata/`](./bigdata) | 大数据全栈：HDFS + MySQL + Hive + Spark + Trino + Iceberg + Paimon | `cd bigdata && ./bootstrap.sh` |
| [`doris/`](./doris)     | Apache Doris 集群：1 FE + 2 BE + 内置 MySQL，支持 MySQL/Hive/Paimon 外表联邦查询 | `cd doris && ./bootstrap.sh`   |
| [`fdb/`](./fdb)         | FoundationDB 集群：3 节点 × 2 进程，double 副本，含自动 E2E | `cd fdb && ./bootstrap.sh`     |
| [`hermes/`](./hermes)   | Hermes Agent 网关 + Dashboard                             | `cd hermes && ./bootstrap.sh`  |
| [`kerberos/`](./kerberos) | Kerberos 实验室：KDC + GSSAPI demo 服务 + 客户端，含自动 E2E | `cd kerberos && ./bootstrap.sh` |
| [`monitor/`](./monitor) | Prometheus + Grafana 监控栈                               | `cd monitor && ./bootstrap.sh` |
| [`mysql/`](./mysql)     | 轻量独立 MySQL（快速实验用）                              | `cd mysql && ./bootstrap.sh`   |
| [`minidfs/`](./minidfs) | MiniDFS：1 NameNode + 3 DataNode + MySQL，含自动 E2E      | `cd minidfs && ./bootstrap.sh` |

每个模块都提供基于 `scripts/bootstrap-common.sh` 的 `bootstrap.sh` 一键引导脚本，统一处理依赖检查、随机凭证生成、端口绑定模式、幂等启动和重置。首次运行会按模块需要提示设置密码（直接回车即使用随机生成值，非交互环境下自动使用随机值）；生成的 `.env` 权限为 `600`，后续重复执行不会覆盖。通用参数：

- `--no-start`：只准备环境（生成 `.env`、下载依赖等），不启动 `docker compose`
- `--reset`：清除所有 volume 和 `.env`，从头初始化

模块间默认不存在网络互通，例外是 `doris/` 与 `bigdata/` 共享 `doris-bigdata-federation` 外部网络（由任一模块的 `bootstrap.sh` 自动创建），用于 Doris 外表联邦查询访问 Hive Metastore 与 HDFS。其他跨模块访问（例如让 `monitor/` 抓取 `bigdata/` 的指标），把对应服务加入同一 Docker 网络或改用 `host` 网络模式即可。

### 端口绑定地址

所有模块的宿主机端口默认只绑定 **`127.0.0.1`（回环地址）**，即只有本机能访问，不会暴露到局域网/公网。首次初始化时可选择三级绑定模式：

1. **仅本机（默认，推荐）**：`PUBLIC_BIND_ADDR=127.0.0.1`，`INTERNAL_BIND_ADDR=127.0.0.1`。
2. **安全公开**：仅有认证的数据入口绑定 `0.0.0.0`，无认证或内部管理端口仍绑定 `127.0.0.1`。
3. **全部公开**：两类地址均绑定 `0.0.0.0`，只应在可信局域网中使用。

`PUBLIC_BIND_ADDR` 对应 MySQL、Doris SQL、Grafana、Hermes Dashboard 等有认证入口；`INTERNAL_BIND_ADDR` 对应 HDFS/Hive/Spark/Trino、Doris 内部端口、Prometheus、Hermes Gateway 等无认证或内部入口。独立 `mysql/` 只有 `PUBLIC_BIND_ADDR`。

如需修改模式，编辑对应模块的 `.env` 后运行 `docker compose up -d` 重新应用。即使接口有密码，当前环境仍未启用 TLS，不建议直接暴露到公网。

---

## minidfs/

MiniDFS 的容器化集成环境。镜像从 `cpp/pl/minidfs/` 源码使用 Bazel 构建，Compose 启动 1 个 NameNode、3 个 DataNode 和 MySQL。

```bash
cd minidfs
./bootstrap.sh

# Bazel 单元测试 + 镜像构建 + 集群启动 + 文件读写 E2E
./tests/e2e.sh all
```

E2E 覆盖三节点注册、命名空间操作、多块三副本上传、下载内容校验、追加、截断、覆盖、调整副本数和递归删除。失败时保留容器及数据卷；可使用 `./tests/e2e.sh down` 停止集群，或使用 `./tests/e2e.sh reset` 删除数据卷。

默认仅在 `127.0.0.1` 暴露 MySQL `13306` 和 NameNode `19000`，DataNode 只在 Compose 网络内通信。

---

## bigdata/

大数据全栈实验环境，支持 Trino / Spark 跨引擎读写 Iceberg 表、Spark 读写 Paimon 表（Hive Metastore 注册、HDFS 存储），并作为 Doris 集群外表联邦查询的数据源。

### 组件与端口

| 服务              | 镜像                                                   | 宿主机端口                     |
| ----------------- | ------------------------------------------------------ | ------------------------------ |
| HDFS NameNode     | `bde2020/hadoop-namenode:2.0.0-hadoop3.2.1-java8`      | 9870 (Web UI), 9000 (RPC)      |
| HDFS DataNode     | `bde2020/hadoop-datanode:2.0.0-hadoop3.2.1-java8`      | —                              |
| MySQL             | `mysql:8.3`                                            | 3307 (映射自容器 3306)         |
| Hive Metastore    | `apache/hive:4.0.0`                                    | 9083                           |
| HiveServer2       | `apache/hive:4.0.0`                                    | 10000 (Thrift), 10002 (Web UI) |
| Spark Master      | `apache/spark:4.0.2-scala2.13-java17-python3-r-ubuntu` | 8080 (Web UI), 7077 (RPC)      |
| Spark Worker      | `apache/spark:4.0.2-scala2.13-java17-python3-r-ubuntu` | 8081 (Web UI)                  |
| Trino Coordinator | `trinodb/trino:468`                                    | 8099                           |
| Trino Worker      | `trinodb/trino:468`                                    | 8199                           |

### 资源需求

`deploy.resources.limits` 配置的资源上限总计约 **27 GB 内存** 和 **19 CPU 核心**（不是启动后的固定占用，可按本机资源调整）。

### 启动步骤

提供一键引导脚本 `bootstrap.sh`，自动完成：检查依赖 → 交互式生成 `.env`（MySQL 密码可自行输入，直接回车使用随机生成值）→ 下载 MySQL 驱动 → 下载 Iceberg / Paimon jar → 创建跨模块联邦网络 `doris-bigdata-federation` → 启动服务 → 通过 spark-sql 幂等写入联邦查询示例数据（Hive `demo.users`、Paimon `paimon.demo.events`）。所有步骤幂等，可安全重复执行。

```bash
cd bigdata
./bootstrap.sh
```

脚本默认会启动服务；如只想准备环境不启动，加 `--no-start`：

```bash
./bootstrap.sh --no-start
```

<details>
<summary>手动逐步执行（脚本的等价展开）</summary>

```bash
cd bigdata

# 1. 复制环境变量模板，并为所有空值填写非空密码/密钥
# 推荐直接运行 ./bootstrap.sh 自动安全生成；以下仅展示手动流程
cp .env.example .env
chmod 600 .env

# 2. 下载 MySQL 驱动（Hive Metastore 依赖，已内置则跳过）
cd hive/lib
curl -O https://repo1.maven.org/maven2/com/mysql/mysql-connector-j/8.3.0/mysql-connector-j-8.3.0.jar
cd ../..

# 3. 下载 Iceberg Spark 运行时 jar
curl -fL -o spark/jars/iceberg-spark-runtime-4.0_2.13-1.10.0.jar \
  https://repo1.maven.org/maven2/org/apache/iceberg/iceberg-spark-runtime-4.0_2.13/1.10.0/iceberg-spark-runtime-4.0_2.13-1.10.0.jar

# 4. 启动所有服务
docker compose up -d

# 5. 初始化 HDFS warehouse 目录（bootstrap.sh 会自动执行）
docker compose exec namenode hdfs dfs -mkdir -p /user/hive/warehouse /tmp
docker compose exec namenode hdfs dfs -chmod -R 777 /user/hive/warehouse /tmp

# 6. 查看状态
docker compose ps
```

</details>

### Web UI

| 服务              | URL                    |
| ----------------- | ---------------------- |
| HDFS NameNode     | http://localhost:9870  |
| HiveServer2       | http://localhost:10002 |
| Spark Master      | http://localhost:8080  |
| Spark Worker      | http://localhost:8081  |
| Trino Coordinator | http://localhost:8099  |

### 连接方式

```bash
# HiveServer2 (Beeline)
beeline -u jdbc:hive2://localhost:10000

# Trino CLI（无密码认证，本地开发环境）
trino --server localhost:8099

# Spark（从宿主机）
spark-shell --master spark://localhost:7077
spark-sql --master spark://localhost:7077
```

### Apache Iceberg

Iceberg 作为湖仓表格式集成到 Trino 和 Spark 中，**无需独立服务**——使用 Hive Metastore 作为 catalog，HDFS 作为存储层。跨引擎验证的核心能力：Trino 创建的表，Spark 可以直接读写，反之亦然。

```sql
-- Trino: 通过 iceberg catalog 创建表
CREATE TABLE iceberg.default.orders (
    order_id BIGINT,
    user_id  BIGINT,
    amount   DOUBLE,
    ts       TIMESTAMP
) WITH (partitioning = ARRAY['days(ts)']);

-- 写入数据
INSERT INTO iceberg.default.orders VALUES (1, 100, 29.99, TIMESTAMP '2026-06-20 10:00:00');

-- Time Travel: 回到历史快照
SELECT * FROM iceberg.default.orders FOR VERSION AS OF 12345;

-- 查看表元数据
SELECT * FROM iceberg.default."orders$snapshots";
SELECT * FROM iceberg.default."orders$files";
```

```sql
-- Spark SQL: 同一张 Iceberg 表
USE iceberg;
SELECT user_id, SUM(amount) FROM iceberg.default.orders GROUP BY user_id;

-- CALL 存储过程管理表
CALL iceberg.system.rewrite_data_files('default.orders');
CALL iceberg.system.expire_snapshots('default.orders', TIMESTAMP '2026-06-01 00:00:00');
```

### 配置文件

| 文件/目录                    | 用途                                                         |
| ---------------------------- | ------------------------------------------------------------ |
| `.env`                       | 敏感信息（密码、密钥），不提交到 Git                         |
| `.env.example`               | 环境变量模板，可提交到 Git                                   |
| `hadoop.env`                 | Hadoop 核心 / HDFS 配置（bde2020 镜像格式）                  |
| `hive/conf/`                 | Hive 配置文件（hive-site.xml、core-site.xml、hdfs-site.xml） |
| `hive/lib/`                  | Hive 依赖 jar（MySQL 驱动，gitignore）                       |
| `spark/conf/`                | Spark 配置（spark-defaults.conf）                            |
| `spark/jars/`                | Spark 框架依赖 jar（Iceberg / Paimon runtime，gitignore）            |
| `extensions/spark/jars/`     | 自定义 Spark DataSource、SQL Extension、UDF jar（gitignore） |
| `trino/etc-coordinator/`     | Trino Coordinator 配置                                       |
| `trino/etc-worker1/`         | Trino Worker 配置                                            |
| `trino/Dockerfile`           | 将自定义 Trino Plugin 构建到所有 Trino 节点                  |
| `extensions/trino/plugins/`  | Trino Plugin，每个插件使用独立子目录（gitignore）            |
| `extensions/trino/catalogs/` | Coordinator/Worker 共用的 Trino Catalog 配置                 |

### Spark 与 Trino 扩展

扩展源码位于 monorepo 的 `java/pl/bigdata/`，以 Bazel 为主构建系统，同时保留 Maven 构建。Bazel 依赖由 `MODULE.bazel` 和 `maven_install.json` 锁定；Trino Plugin 发布包使用 `rules_pkg` 的 `pkg_files` 和 `pkg_zip` 生成，以保留插件子目录结构。

在仓库根目录构建、测试扩展：

```bash
bazel test //java/pl/bigdata:tests
bazel build \
  //java/pl/bigdata:spark_extension_jar \
  //java/pl/bigdata:trino_plugin_zip
```

构建产物：

- `bazel-bin/java/pl/bigdata/spark-extensions/spark-extensions.jar`
- `bazel-bin/java/pl/bigdata/trino-extensions/e2e-functions.zip`

Trino ZIP 解压后的结构为 `e2e-functions/trino-extensions.jar`，可直接投放到 `bigdata/extensions/trino/plugins/`。Spark 扩展使用 `--release 17` 构建，兼容当前 Spark Java 17 运行时。

Spark 自定义 jar 放到 `bigdata/extensions/spark/jars/` 后，重建 Spark Master 和 Worker：

```bash
cd bigdata
docker compose up -d --force-recreate spark-master spark-worker
```

目录已加入 Driver 和 Executor 的 classpath。DataSource 可在 SQL 中通过短名称或实现类全名加载；UDF、`SparkSessionExtensions` 和自定义 Catalog 仍需按 Spark 机制注册或配置。扩展须匹配 Spark 4.0.2、Scala 2.13 和 Java 17。

Trino Plugin 须按 Trino 468 SPI 构建，将插件发布包解压到独立子目录，例如 `extensions/trino/plugins/my-plugin/*.jar`。不要将 jar 直接放在 `plugins/` 根目录，也不要放入非 jar 文件。运行以下命令会构建派生镜像，并在 Coordinator 和 Worker 上加载同一组插件：

```bash
cd bigdata
docker compose build trino-coordinator
docker compose up -d --force-recreate trino-coordinator trino-worker-1
```

Connector 对应的 Catalog 配置放在 `extensions/trino/catalogs/<catalog>.properties`。Trino 与 PrestoDB 的 Plugin SPI 不兼容，当前环境只支持 Trino 468 插件。

### 测试

扩展源码位于 `../java/pl/bigdata/`，以 Bazel 为主构建系统，同时保留 Maven 构建。以下命令默认使用 Bazel 构建并测试 Spark（Java 17 bytecode）和 Trino 468 扩展，投放构建产物、重建集群、执行 SQL 断言并清理测试数据与投放产物：

```bash
cd bigdata
./tests/e2e.sh all
```

也可分阶段执行 `build`、`start`、`test`、`clean`、`down` 或 `reset`。默认 `BUILD_SYSTEM=bazel`；如需验证备用 Maven 构建，可执行 `BUILD_SYSTEM=maven ./tests/e2e.sh all`，该模式使用固定 Maven/JDK 23 容器。失败时脚本保留容器和 volume，便于通过 `docker compose logs` 排查。

各组件独立验证及 HDFS/Hive/Spark/Trino/Iceberg 跨引擎读写、多组件组合场景见 [`bigdata/TESTING.md`](./bigdata/TESTING.md)。

### 已知限制

- HDFS 单副本（`dfs.replication=1`），DataNode 故障数据即不可用
- 未部署 YARN（ResourceManager/NodeManager），Hive 使用 Tez local mode
- Trino 未启用认证（本地开发环境），如需认证请参考 Trino 文档配置 TLS + PASSWORD 认证
- MySQL 明文密码存储在 `.env`，仅适合本地开发（端口默认仅绑定 `127.0.0.1`，见[端口绑定地址](#端口绑定地址)）

---

## doris/

Apache Doris 实验集群：1 个 FE（Frontend）+ 2 个 BE（Backend）+ 1 个内置 MySQL（外表数据源），用于本地体验 MPP 分布式查询、多副本存储与外表联邦查询。

### 组件与端口

| 服务 | 镜像                               | 宿主机端口                                         |
| ---- | ---------------------------------- | -------------------------------------------------- |
| FE   | `apache/doris:fe-${DORIS_VERSION}` | 8030 (HTTP/控制台), 9030 (MySQL 协议), 9010 (选举) |
| BE1  | `apache/doris:be-${DORIS_VERSION}` | 8040 (Web), 9050 (心跳)                            |
| BE2  | `apache/doris:be-${DORIS_VERSION}` | 8041 (Web), 9051 (心跳)                            |
| MySQL | `mysql:8.3`                       | 3308 (联邦查询内置数据源，仅本机调试)               |

默认 `DORIS_VERSION=4.0.6`（可通过环境变量覆盖），镜像为官方多架构 manifest，自动适配 x86_64 / arm64。`deploy.resources.limits` 配置的资源上限总计约 **6 GB 内存** 和 **6 CPU 核心**。

### 启动步骤

```bash
cd doris
./bootstrap.sh
```

脚本会依次：检查 Docker 依赖 → 生成/补齐 `.env`（含内置 MySQL 密码）→ 准备 JDBC 驱动（优先复用 `bigdata/hive/lib/` 的 MySQL 驱动）→ 创建跨模块联邦网络 `doris-bigdata-federation` → 启动 FE/BE/MySQL 容器 → 轮询等待 FE 选主完成 → 轮询等待 2 个 BE 注册存活 → 校验示例表数据是否就绪 → 创建并验证外表 Catalog → 设置 root 密码。首次启动因需拉起 JVM 和完成选举，约需 1-2 分钟。

只做前置检查、不启动服务：

```bash
./bootstrap.sh --no-start
```

### 连接方式

```bash
mysql -h 127.0.0.1 -P 9030 -u root -p
# 密码见 doris/.env 的 DORIS_ROOT_PASSWORD
```

FE 控制台：http://localhost:8030

### 示例数据

`init-sql/01-init.sql` 会在 BE 容器首次启动时自动执行一次（挂载到 `/docker-entrypoint-initdb.d`），创建 `demo.orders` 表并写入几条示例数据（`replication_allocation` 设为 2，正好用两个 BE 体验多副本存储）：

```sql
SELECT * FROM demo.orders;
SHOW BACKENDS\G
```

如需重新执行初始化脚本，需先 `./bootstrap.sh --reset` 清空数据卷再重新启动（脚本仅在存储目录为空时执行一次）。

### 外表联邦查询

`bootstrap.sh` 会自动创建并验证以下外表 Catalog（所有步骤幂等，可重跑补齐）：

| Catalog      | 类型     | 数据源                                                        |
| ------------ | -------- | ------------------------------------------------------------- |
| `mysql_fed`  | `jdbc`   | 内置 MySQL `federation_demo.users`                            |
| `hive_fed`   | `hms`    | bigdata 模块 Hive Metastore `demo.users`（数据在 HDFS）        |
| `paimon_fed` | `paimon` | bigdata 模块 Paimon `demo.events`（HMS 注册，数据在 HDFS）     |

`hive_fed` / `paimon_fed` 依赖 bigdata 模块运行（两模块共享 `doris-bigdata-federation` 外部网络）；bigdata 未启动时自动跳过，先执行 `bigdata/bootstrap.sh` 再重跑本模块 bootstrap 即可补齐。

```sql
-- 内表 ⨝ 湖仓外表联邦 JOIN
SELECT o.order_id, o.amount, u.user_name, u.vip_level
FROM demo.orders o
JOIN hive_fed.demo.users u ON o.user_id = u.user_id;

-- 直查 Paimon / MySQL 外表
SELECT * FROM paimon_fed.demo.events;
SELECT * FROM mysql_fed.federation_demo.users;
```

> 注意：Doris 官方镜像只在首次初始化时写入 `priority_networks`，双网络（doris + federation）下重启会误选网卡导致 FE 无法选主。compose 中已为 FE/BE 通过 `command` 兜底写入 `priority_networks = 172.28.10.0/24`，勿随意移除。

### 测试

更完整的功能验证（建表模型、Stream Load 导入、物化视图、多副本高可用、Schema Change 等）见 [`doris/TESTING.md`](./doris/TESTING.md)。

### 已知限制

- 组网依赖固定 IP（`172.28.10.0/24` 网段），如与本机其他 Docker 网络冲突，请修改 `docker-compose.yml` 中的 `subnet` 与各服务 `ipv4_address`
- Hive/Paimon 外表 Catalog 依赖 bigdata 模块，bigdata 停止后相关查询不可用（`mysql_fed` 不受影响）
- SQL 查询端口 `9030` 使用 root 密码认证，但 HTTP/内部端口仍可能包含无认证接口；所有入口均无 TLS，仅适合开发测试
- FE 单节点，无高可用；如需体验多 FE 选举，可仿照 `FE_SERVERS` 格式自行扩展

---

## fdb/

FoundationDB 实验集群：3 个节点容器，每节点由 `fdbmonitor`（生产同款进程监管）托管 2 个 `fdbserver` 进程，共 6 进程；3 个 coordinator，`double` 副本 + `ssd`（SQLite）引擎，数据持久化在命名卷。

| 服务         | 容器内地址             | 角色                                          |
| ------------ | ---------------------- | --------------------------------------------- |
| fdb-node-1   | 172.28.11.11:4500/4501 | coordinator + storage/tlog 等                 |
| fdb-node-2   | 172.28.11.12:4500/4501 | coordinator + storage/tlog 等                 |
| fdb-node-3   | 172.28.11.13:4500/4501 | coordinator + storage/tlog 等                 |
| fdb-exporter | monitor 网络 :9444     | Prometheus 指标导出（aikoven/foundationdb-exporter） |

FDB 客户端需要直连集群中的**每个**进程（coordinator、proxy、storage server），macOS 宿主机无法路由到容器 IP，且 fdbserver 的双 public address 必须分属不同 TLS 状态，因此本模块不做宿主机端口映射，客户端一律通过容器网络访问：

```bash
cd fdb
./bootstrap.sh        # 启动集群并初始化（configure new double ssd）
./tests/e2e.sh all    # 状态/读写/独立客户端 + 单节点故障容错 + 重启持久化

# 交互式 fdbcli（独立客户端容器，profile 默认不启动）
docker compose run --rm client
docker compose run --rm client --exec "status"

# 节点容器内
docker compose exec fdb-node-1 fdbcli -C /var/fdb/fdb.cluster
```

每个 fdbserver 的 `--memory` 限制为 1GiB（`--cache-memory` 256MiB），单容器资源上限 2.5 GB / 2 CPU。Trace 日志在容器 `/var/fdb/logs/<端口>/`（命名卷持久化）。FDB 未启用认证与 TLS，仅限本地开发测试。

### 监控（Prometheus + Grafana）

指标导出采用社区方案 [`aikoven/foundationdb-exporter`](https://github.com/aikoven/foundationdb-exporter)：通过 FDB Node 客户端实时读取 `\xff\xff/status/json`（每次抓取实时采集），暴露 `:9444/metrics`，并加入 [`monitor/`](./monitor) 模块的共享网络供 Prometheus 抓取（job 名 `foundationdb`）。`bootstrap.sh` 启动前会自动检测并在需要时拉起 monitor 模块。

Grafana 内置配套仪表盘 **FoundationDB**（40 面板：可用性、事务/读写速率、QoS 限流、数据迁移、commit/GRV proxy 延迟 p50-p99 分布、storage 读延迟、latency probe、进程资源等）。exporter 未映射宿主机端口，容器网络内验证：

```bash
docker compose exec fdb-node-1 wget -q -O- http://fdb-exporter:9444/metrics | head
```

注意事项：

- 镜像仅发布 **amd64**，Apple Silicon 需 Rosetta 模拟运行
- **必须用 `3.1.0` tag**：`latest` 指向 2024-11 的旧构建（内置 FDB 7.1 客户端，连不上 7.3 集群，报 `IncompatibleProtocolVersion`）；`3.1.0`（2025-10）内置 7.3.56 客户端
- exporter 容器的 FDB 客户端 trace 日志在 `/tmp/trace`（tmpfs），排障用：`docker exec fdb-exporter ls /tmp/trace`

---

## hermes/

Hermes Agent 网关服务，使用 `nousresearch/hermes-agent:latest` 镜像，提供 API 网关和 Dashboard；资源上限为 **4 GB 内存** 和 **2 CPU 核心**。

| 服务        | 端口 | 用途                                                            |
| ----------- | ---- | --------------------------------------------------------------- |
| Gateway API | 8642 | Hermes 网关接口（当前镜像版本可能不监听该端口，以容器日志为准） |
| Dashboard   | 9119 | Web 管理界面（登录认证）                                        |

```bash
cd hermes
./bootstrap.sh   # 首次运行会交互式设置账号密码，直接回车使用随机默认值
```

Dashboard 默认开启（`HERMES_DASHBOARD=1`），使用登录会话认证，账号密码由 `.env` 中的 `HERMES_DASHBOARD_BASIC_AUTH_USERNAME` / `HERMES_DASHBOARD_BASIC_AUTH_PASSWORD` 提供（`bootstrap.sh` 首次运行时交互式生成）。当前镜像已验证 Dashboard 可登录；若 `8642` 无法连接，请检查 `docker compose logs hermes` 是否实际启动了 Gateway HTTP 监听。

---

## kerberos/

Kerberos 学习实验室：MIT KDC（realm `LAB.LOCAL`）+ GSSAPI echo demo 服务 + 客户端容器，用于体验 `kinit → TGT → 服务票据 → 双向认证 → 会话密钥加密` 全流程以及 keytab 免密认证。

```bash
cd kerberos
./bootstrap.sh        # 启动并打印快速玩法
./tests/e2e.sh all    # 8 项断言：密码认证、预认证拒绝、双向认证、服务票据、无凭证拒绝、keytab、多用户、审计日志
```

预置用户 `alice/alice123`、`bob/bob12345`，管理员 `admin/admin@LAB.LOCAL`（`admin123`），服务主体 `demo/demo-server@LAB.LOCAL`（keytab 认证）。KDC 的 88 端口映射到 `127.0.0.1`，宿主机配好 `krb5.conf` 后可直接 `kinit`。详细测试说明见 [`kerberos/TESTING.md`](./kerberos/TESTING.md)。

---

## monitor/

Prometheus + Grafana 监控栈。

| 服务       | 镜像                      | 端口 | 用途           |
| ---------- | ------------------------- | ---- | -------------- |
| Prometheus | `prom/prometheus:latest` | 9090 | 指标采集与查询 |
| Grafana    | `grafana/grafana:latest`  | 3000 | 可视化面板     |

```bash
cd monitor && ./bootstrap.sh
```

Prometheus 抓取配置见 `prometheus/prometheus.yml`：除自身外，默认抓取 [`fdb/`](./fdb) 模块的 `fdb-exporter:9444`（job `foundationdb`，fdb 未启动时该 target 显示 DOWN 不影响其他 job），可按需继续添加 target。

Grafana 通过 `grafana/provisioning/` 自动配置：Prometheus 数据源（uid `prometheus`）与 `grafana/dashboards/` 下的仪表盘（当前含 **FoundationDB**，来自 `aikoven/foundationdb-exporter` 仓库自带面板）。Grafana 用户名默认为 `admin`，随机密码由 `bootstrap.sh` 写入 `monitor/.env` 的 `GRAFANA_ADMIN_PASSWORD`；Prometheus 默认仅绑定本机且未启用认证。

其他模块接入监控的方式：服务加入本模块的 `monitor_monitor` 网络（`external: true`），再在 `prometheus/prometheus.yml` 添加对应 job 即可（`fdb/` 模块即采用该方式，其 `bootstrap.sh` 会在网络不存在时自动拉起本模块）。

---

## mysql/

轻量独立 MySQL 实例，用于快速数据库实验（不依赖大数据组件）。

| 服务      | 端口                   |
| --------- | ---------------------- |
| MySQL 8.3 | 3307 (映射自容器 3306) |

```bash
cd mysql
./bootstrap.sh   # 首次运行会交互式设置 root 密码，直接回车使用随机默认值

# 连接（宿主机端口 3307，避免与本机 MySQL 冲突）
mysql -h 127.0.0.1 -P 3307 -u root -p
```

root 密码由 `.env` 提供（`bootstrap.sh` 首次运行时交互式生成），数据持久化在 `mysql-data` 命名卷。`mysql/` 与 `bigdata/` 内置 MySQL 都默认映射宿主机 `3307`，因此不能同时使用默认端口启动；需要并行运行时请修改其中一个模块的端口映射。

---

## 常用命令

```bash
# 启动某个模块（首次运行会交互式设置密码）
cd <module> && ./bootstrap.sh

# 只准备环境，不启动服务
./bootstrap.sh --no-start

# 完全重置（清除 volume 和 .env，从头开始）
./bootstrap.sh --reset

# 查看日志
docker compose logs -f <service>

# 停止并保留数据
docker compose down

# 停止并清除数据卷（谨慎！）
docker compose down -v
```
