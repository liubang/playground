# Docker 实验环境

本地开发/测试用的容器化实验环境。每个子目录是一个**独立的实验模块**，各自维护 `docker-compose.yml`、网络和数据卷，互不依赖，按需单独启动。

> ⚠️ 仅适用于开发/测试环境，不可用于生产。HDFS 单副本、明文密码、无 TLS 等配置不适合生产部署。

## 模块一览

| 模块                    | 说明                                                      | 启动命令                       |
| ----------------------- | --------------------------------------------------------- | ------------------------------ |
| [`bigdata/`](./bigdata) | 大数据全栈：HDFS + MySQL + Hive + Spark + Trino + Iceberg | `cd bigdata && ./bootstrap.sh` |
| [`doris/`](./doris)     | Apache Doris 集群：1 FE + 2 BE                            | `cd doris && ./bootstrap.sh`   |
| [`hermes/`](./hermes)   | Hermes Agent 网关 + Dashboard                             | `cd hermes && ./bootstrap.sh`  |
| [`monitor/`](./monitor) | Prometheus + Grafana 监控栈                               | `cd monitor && ./bootstrap.sh` |
| [`mysql/`](./mysql)     | 轻量独立 MySQL（快速实验用）                              | `cd mysql && ./bootstrap.sh`   |
| [`minidfs/`](./minidfs) | MiniDFS：1 NameNode + 3 DataNode + MySQL，含自动 E2E      | `cd minidfs && ./bootstrap.sh` |

每个模块都提供基于 `scripts/bootstrap-common.sh` 的 `bootstrap.sh` 一键引导脚本，统一处理依赖检查、随机凭证生成、端口绑定模式、幂等启动和重置。首次运行会按模块需要提示设置密码（直接回车即使用随机生成值，非交互环境下自动使用随机值）；生成的 `.env` 权限为 `600`，后续重复执行不会覆盖。通用参数：

- `--no-start`：只准备环境（生成 `.env`、下载依赖等），不启动 `docker compose`
- `--reset`：清除所有 volume 和 `.env`，从头初始化

模块间默认不存在网络互通，若需要跨模块访问（例如让 `monitor/` 抓取 `bigdata/` 的指标），把对应服务加入同一 Docker 网络或改用 `host` 网络模式即可。

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

大数据全栈实验环境，支持 Trino / Spark 跨引擎读写 Iceberg 表。

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

提供一键引导脚本 `bootstrap.sh`，自动完成：检查依赖 → 交互式生成 `.env`（MySQL 密码可自行输入，直接回车使用随机生成值）→ 下载 MySQL 驱动 → 下载 Iceberg jar → 启动服务。所有步骤幂等，可安全重复执行。

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
| `spark/jars/`                | Spark 框架依赖 jar（Iceberg runtime，gitignore）             |
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

Apache Doris 实验集群：1 个 FE（Frontend）+ 2 个 BE（Backend），用于本地体验 MPP 分布式查询和多副本存储。

### 组件与端口

| 服务 | 镜像                               | 宿主机端口                                         |
| ---- | ---------------------------------- | -------------------------------------------------- |
| FE   | `apache/doris:fe-${DORIS_VERSION}` | 8030 (HTTP/控制台), 9030 (MySQL 协议), 9010 (选举) |
| BE1  | `apache/doris:be-${DORIS_VERSION}` | 8040 (Web), 9050 (心跳)                            |
| BE2  | `apache/doris:be-${DORIS_VERSION}` | 8041 (Web), 9051 (心跳)                            |

默认 `DORIS_VERSION=4.0.6`（可通过环境变量覆盖），镜像为官方多架构 manifest，自动适配 x86_64 / arm64。`deploy.resources.limits` 配置的资源上限总计约 **6 GB 内存** 和 **6 CPU 核心**。

### 启动步骤

```bash
cd doris
./bootstrap.sh
```

脚本会依次：检查 Docker 依赖 → 启动 FE/BE 容器 → 轮询等待 FE 选主完成 → 轮询等待 2 个 BE 注册存活 → 校验示例表数据是否就绪。首次启动因需拉起 JVM 和完成选举，约需 1-2 分钟。

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

### 测试

更完整的功能验证（建表模型、Stream Load 导入、物化视图、多副本高可用、Schema Change 等）见 [`doris/TESTING.md`](./doris/TESTING.md)。

### 已知限制

- 组网依赖固定 IP（`172.28.10.0/24` 网段），如与本机其他 Docker 网络冲突，请修改 `docker-compose.yml` 中的 `subnet` 与各服务 `ipv4_address`
- SQL 查询端口 `9030` 使用 root 密码认证，但 HTTP/内部端口仍可能包含无认证接口；所有入口均无 TLS，仅适合开发测试
- FE 单节点，无高可用；如需体验多 FE 选举，可仿照 `FE_SERVERS` 格式自行扩展

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

## monitor/

Prometheus + Grafana 监控栈。

| 服务       | 镜像                      | 端口 | 用途           |
| ---------- | ------------------------- | ---- | -------------- |
| Prometheus | `prom/prometheus:v3.13.0` | 9090 | 指标采集与查询 |
| Grafana    | `grafana/grafana:13.1.0`  | 3000 | 可视化面板     |

```bash
cd monitor && ./bootstrap.sh
```

Prometheus 抓取配置见 `prometheus/prometheus.yml`，当前仅抓取自身，可按需添加 target。Grafana 用户名默认为 `admin`，随机密码由 `bootstrap.sh` 写入 `monitor/.env` 的 `GRAFANA_ADMIN_PASSWORD`；Prometheus 默认仅绑定本机且未启用认证。

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
