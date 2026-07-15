# Bigdata 模块测试指南

本文档覆盖 bigdata 环境中各个组件的独立验证，以及组件间的组合使用场景测试。

> **前置条件**：已通过 `./bootstrap.sh` 启动所有服务，且 `docker compose ps` 显示全部 healthy。

> **约定**：各章节自包含（自带建表和清理），可独立执行。多组件场景（第 7 节）也是自包含的。执行非交互 MySQL 命令前可加载密码：`export MYSQL_ROOT_PASSWORD=$(grep '^MYSQL_ROOT_PASSWORD=' .env | cut -d= -f2-)`。

## 自动化扩展 E2E

从 `docker/bigdata/` 执行 `./tests/e2e.sh all`，默认使用 Bazel 完成单元测试和构建，再执行 Spark/Trino 扩展投放、集群重建、DataNode 与服务就绪检查、SQL 断言以及测试数据和投放产物清理。扩展源码位于 `../../java/pl/bigdata/`。如需验证备用 Maven 构建，可执行 `BUILD_SYSTEM=maven ./tests/e2e.sh all`，该模式使用固定 Maven/JDK 23 容器。

支持的子命令为 `build`、`start`、`test`、`clean`、`down`、`reset`。测试失败时不会关闭容器或删除 volume，以保留诊断现场。

## 目录

- [0. 环境准备与连通性检查](#0-环境准备与连通性检查)
- [1. HDFS 测试](#1-hdfs-测试)
- [2. MySQL 测试](#2-mysql-测试)
- [3. Hive 测试](#3-hive-测试)
- [4. Spark 测试](#4-spark-测试)
- [5. Trino 测试](#5-trino-测试)
- [6. Iceberg 跨引擎测试](#6-iceberg-跨引擎测试)
- [7. 多组件组合场景](#7-多组件组合场景)
- [附录：排障命令与常见问题](#附录排障命令与常见问题)

---

## 0. 环境准备与连通性检查

### 0.1 确认所有服务健康

```bash
cd bigdata
docker compose ps
```

预期：带健康检查的服务显示 `Up (healthy)`，其余服务显示 `Up`。首次启动需等待 1-2 分钟（各服务健康检查和依赖链会依次启动）。

### 0.2 端口连通性检查

```bash
# HDFS NameNode Web UI
curl -s -o /dev/null -w "%{http_code}" http://localhost:9870
# 预期: 200

# HiveServer2 Thrift (macOS 自带 nc)
nc -z localhost 10000 && echo "OK"
# 预期: OK

# Hive Metastore
nc -z localhost 9083 && echo "OK"
# 预期: OK

# Spark Master Web UI
curl -s -o /dev/null -w "%{http_code}" http://localhost:8080
# 预期: 200

# Trino Coordinator
curl -s -o /dev/null -w "%{http_code}" http://localhost:8099/v1/info
# 预期: 200

# MySQL (宿主机映射端口 3307)
nc -z localhost 3307 && echo "OK"
# 预期: OK
```

### 0.3 连接信息速查

| 组件           | 连接方式                                                         | 认证                                   |
| -------------- | ---------------------------------------------------------------- | -------------------------------------- |
| HDFS NameNode  | `hdfs://namenode:9000`（容器内）/ Web UI `http://localhost:9870` | 无                                     |
| MySQL          | `mysql -h 127.0.0.1 -P 3307 -u root -p`                          | 密码见 `.env` 的 `MYSQL_ROOT_PASSWORD` |
| Hive Metastore | `thrift://hivemetastore:9083`（容器内）                          | 无                                     |
| HiveServer2    | `beeline -u jdbc:hive2://localhost:10000`                        | 无                                     |
| Spark Master   | `spark://localhost:7077`                                         | 无                                     |
| Trino          | `trino --server localhost:8099`                                  | 无（本地开发环境）                     |

### 0.4 CLI 工具说明

本文档中部分命令需要宿主机安装对应 CLI（`mysql`、`trino`）。如果未安装，可使用当前镜像内置的客户端替代：

```bash
# MySQL → 用容器内的 mysql 客户端（交互式输入 .env 中的密码）
docker compose exec mysql mysql -h 127.0.0.1 -u root -p

# Trino → 当前 trinodb/trino:468 镜像内置 trino CLI
docker compose exec trino-coordinator trino --server http://127.0.0.1:8099
```

---

## 1. HDFS 测试

### 1.1 Web UI 验证

浏览器打开 `http://localhost:9870`，确认：

- Overview 页面显示 `Cluster ID: test`
- Datanodes 页面有 1 个 DataNode 在线
- `dfs.replication = 1`，容量正常

### 1.2 文件操作

```bash
# 进入 namenode 容器
docker exec -it namenode bash

# 创建目录
hdfs dfs -mkdir -p /test/input

# 写入文件
echo "hello hadoop" > /tmp/sample.txt
hdfs dfs -put /tmp/sample.txt /test/input/

# 查看文件
hdfs dfs -ls /test/input/
# 预期: -rw-r--r-- 1 root supergroup 13 ... /test/input/sample.txt

hdfs dfs -cat /test/input/sample.txt
# 预期: hello hadoop

# 查看文件块位置（验证 DataNode 存储）
hdfs fsck /test/input/sample.txt -files -blocks -locations
# 预期: 1 个 block，副本数 1，存储在 datanode 上

# 删除测试数据
hdfs dfs -rm -r /test

exit
```

### 1.3 验证 Hive Warehouse 目录

```bash
docker exec -it namenode hdfs dfs -ls /user/hive/warehouse
# 预期: 目录存在（由 Hive Metastore 初始化时创建）
```

---

## 2. MySQL 测试

MySQL 在本环境中承担两个角色：Hive Metastore 的元数据存储，以及通过 Trino `mysql` catalog 可查询的数据源。

### 2.1 连接 MySQL

```bash
# 方式一：宿主机 mysql 客户端
mysql -h 127.0.0.1 -P 3307 -u root -p
# 输入 .env 中的 MYSQL_ROOT_PASSWORD

# 方式二：容器内客户端
docker compose exec mysql mysql -h 127.0.0.1 -u root -p
```

### 2.2 验证 Hive Metastore 元数据

```sql
-- metastore 数据库由 bootstrap 创建
SHOW DATABASES;
-- 预期: information_schema, metastore, mysql, performance_schema, sys

USE metastore;

-- Hive schematool 初始化后应有 300+ 张元数据表
SHOW TABLES;

-- 验证 schema 版本
SELECT * FROM VERSION;
-- 预期: SCHEMA_VERSION = 4.0.0, VER_ID = 1

-- 查看已注册的数据库（初始只有 default）
SELECT DB_ID, NAME, DB_LOCATION_URI FROM DBS;
-- 预期: 1, default, hdfs://namenode:9000/user/hive/warehouse

exit;
```

### 2.3 创建业务数据（供 Trino 联邦查询使用）

```sql
-- 创建业务数据库
CREATE DATABASE IF NOT EXISTS biz;

USE biz;

-- 用户表
CREATE TABLE users (
    id   INT PRIMARY KEY,
    name VARCHAR(100),
    dept VARCHAR(50)
);

INSERT INTO users VALUES
    (100, 'Alice', 'Engineering'),
    (101, 'Bob',   'Sales'),
    (102, 'Carol', 'Marketing');

SELECT * FROM users;

exit;
```

> ⚠️ **权限设置**：Trino 通过 MySQL catalog 连接时使用 `hive` 用户，默认无权访问 `root` 创建的 `biz` 库。需执行以下授权：
>
> ```sql
> GRANT ALL PRIVILEGES ON biz.* TO 'hive'@'%';
> FLUSH PRIVILEGES;
> ```

> 这张 `biz.users` 表会在 [第 7.3 节](#73-trino-联邦查询)的跨数据源 JOIN 中使用。

### 2.4 通过 Trino 访问 MySQL

```bash
trino --server localhost:8099
```

```sql
SHOW CATALOGS;
-- 预期: hive, iceberg, mysql, system

SHOW SCHEMAS IN mysql;
-- 预期: information_schema, biz, metastore, ...

-- 查询刚创建的业务数据
SELECT * FROM mysql.biz.users;
-- 预期: 3 行数据

-- 也可以查询 Hive 元数据（注意：Trino 将标识符规范化为小写）
SELECT db_id, name FROM mysql.metastore.dbs;
```

---

## 3. Hive 测试

### 3.1 通过 Beeline 连接

```bash
docker exec -it hiveserver2 beeline -u "jdbc:hive2://localhost:10000"
```

### 3.2 基础 DDL/DML

```sql
CREATE DATABASE IF NOT EXISTS test_db;
USE test_db;

-- 内部表（TEXTFILE 格式）
CREATE TABLE employees (
    id     INT,
    name   STRING,
    dept   STRING,
    salary DOUBLE
)
ROW FORMAT DELIMITED
FIELDS TERMINATED BY ',';

-- 插入数据（Tez local mode）
INSERT INTO employees VALUES
    (1, 'Alice',   'Engineering', 95000.0),
    (2, 'Bob',     'Sales',       72000.0),
    (3, 'Charlie', 'Engineering', 88000.0),
    (4, 'Diana',   'Marketing',   68000.0);

SELECT * FROM employees ORDER BY salary DESC;
-- 预期: Alice(95000) → Charlie(88000) → Bob(72000) → Diana(68000)

SELECT dept, COUNT(*) AS cnt, AVG(salary) AS avg_salary
FROM employees
GROUP BY dept;
-- 预期: Engineering 2 人 91500, Sales 1 人 72000, Marketing 1 人 68000

!exit;
```

### 3.3 验证 HDFS 物理存储

```bash
docker exec -it namenode hdfs dfs -ls /user/hive/warehouse/test_db.db/employees
# 预期: 有数据文件（类似 000000_0）
```

### 3.4 验证 Metastore 元数据

```bash
# 退出 beeline 后执行
MYSQL_PWD="$MYSQL_ROOT_PASSWORD" mysql -h 127.0.0.1 -P 3307 -u root -e "
  SELECT TBL_NAME, TBL_TYPE, LOCATION
  FROM metastore.TBLS t
  JOIN metastore.DBS d ON t.DB_ID = d.DB_ID
  WHERE d.NAME = 'test_db';
"
# 预期: employees, EXTERNAL_TABLE, hdfs://namenode:9000/user/hive/warehouse/test_db.db/employees
# 注意: Hive 4.0 默认将非 ACID 事务表转换为 EXTERNAL_TABLE
```

### 3.5 清理

```bash
docker exec -it hiveserver2 beeline -u "jdbc:hive2://localhost:10000" -e "DROP DATABASE test_db CASCADE;"
```

---

## 4. Spark 测试

### 4.1 Spark Shell 交互测试

```bash
docker exec -it spark-master /opt/spark/bin/spark-shell --master spark://spark-master:7077
```

> 注意：`spark-shell` 和 `spark-sql` 未加入容器 PATH，需使用完整路径 `/opt/spark/bin/`。

```scala
// spark-shell 自动导入 spark.implicits._ 和 org.apache.spark.sql.functions._

// 基础 RDD
val rdd = sc.parallelize(1 to 100)
rdd.sum()       // 预期 5050
rdd.count()     // 预期 100

// DataFrame
val df = spark.createDataFrame(Seq(
  (1, "Alice",   95000),
  (2, "Bob",     72000),
  (3, "Charlie", 88000)
)).toDF("id", "name", "salary")

df.show()
df.filter($"salary" > 80000).show()
// 预期: Alice 和 Charlie

df.agg(avg("salary")).show()
// 预期: 85000.0

// 计算 Pi（替代 spark-submit SparkPi 的方式）
val slices = 10
val n = 100000 * slices
val count = sc.parallelize(1 to n, slices).map { i =>
  val x = math.random
  val y = math.random
  if (x*x + y*y <= 1) 1 else 0
}.reduce(_ + _)
println(s"Pi is roughly ${4.0 * count / n}")
// 预期: 约 3.14

:quit
```

### 4.2 Spark SQL 读写 Hive 表

```bash
docker exec -it spark-master /opt/spark/bin/spark-sql --master spark://spark-master:7077
```

```sql
-- Spark 通过 classpath 上的 hive-site.xml 发现 Hive Metastore
SHOW DATABASES;
-- 预期: default 库（以及 Hive 中已创建的库）

CREATE DATABASE IF NOT EXISTS spark_test;
USE spark_test;

-- 创建 Hive 表（存储到 Hive warehouse）
CREATE TABLE sales (
    id      INT,
    product STRING,
    amount  DOUBLE
) USING hive;

INSERT INTO sales VALUES
    (1, 'Widget', 100.50),
    (2, 'Gadget', 250.00),
    (3, 'Widget',  75.25);

SELECT product, SUM(amount) AS total
FROM sales
GROUP BY product
ORDER BY total DESC;
-- 预期: Gadget 250.00, Widget 175.75

exit;
```

### 4.3 验证 Spark 建的表对 Hive 可见

```bash
# Hive 中查询 Spark 创建的表
docker exec -it hiveserver2 beeline -u "jdbc:hive2://localhost:10000" \
  -e "SELECT * FROM spark_test.sales;"
# 预期: 3 行数据
```

### 4.4 验证 Spark Worker 资源

浏览器打开 `http://localhost:8080`，确认：

- Workers 列表有 1 个 Worker 在线
- Worker 的 Cores 和 Memory 正确识别（4 cores, 8g）

### 4.5 自定义数据格式扩展测试

将按 Spark 4.0.2、Scala 2.13、Java 17 构建的 DataSource jar 放入 `extensions/spark/jars/`，再重建 Master 和 Worker：

```bash
docker compose up -d --force-recreate spark-master spark-worker
```

以下以实现类 `com.example.spark.CustomFileFormat` 为例，通过 CTAS 同时验证 Driver 解析、Executor 写入和回读。若扩展实现了 `DataSourceRegister`，也可将实现类全名替换为其短名称。

```sql
DROP TABLE IF EXISTS default.custom_format_ctas;

CREATE TABLE default.custom_format_ctas
USING com.example.spark.CustomFileFormat
OPTIONS (delimiter='|', header='true')
AS
SELECT id, concat('value-', id) AS value
FROM range(1, 4);

SELECT * FROM default.custom_format_ctas ORDER BY id;
-- 预期: (1,value-1)、(2,value-2)、(3,value-3)

DESCRIBE EXTENDED default.custom_format_ctas;
-- 预期: Provider 为自定义格式实现类或短名称

DROP TABLE default.custom_format_ctas;
```

如 Driver 能找到类但 Executor 报 `ClassNotFoundException`，确认 jar 已挂载到 Spark Worker，并重新创建 Master、Worker 和当前 `spark-sql` 会话。

### 4.6 清理

```bash
docker exec -it spark-master /opt/spark/bin/spark-sql --master spark://spark-master:7077 \
  -e "DROP DATABASE spark_test CASCADE;"
```

---

## 5. Trino 测试

### 5.1 连接 Trino

```bash
# 方式一：宿主机 trino CLI
trino --server localhost:8099

# 方式二：当前 trinodb/trino:468 镜像内置 CLI
docker compose exec trino-coordinator trino --server http://127.0.0.1:8099
```

### 5.2 基础查询

```sql
-- catalogs（hive / iceberg / mysql / system）
SHOW CATALOGS;

-- 节点状态（coordinator + worker）
SELECT node_id, state, coordinator FROM system.runtime.nodes;
-- 预期: 2 行，coordinator-1 (true), worker-1 (false)，state 均 active
```

### 5.3 Trino 通过 Hive catalog 查询表

> ⚠️ **重要限制**：由于 Hive 4.0 Metastore 默认将非 ACID 事务表转换为 `EXTERNAL_TABLE`，而 Trino Hive connector 不允许写入 `EXTERNAL_TABLE`，因此 Trino **只能读取** Hive 表，无法执行 INSERT/UPDATE/DELETE。如需通过 Trino 写入数据，请使用 Iceberg catalog（见第 6 节）。

```sql
-- Trino 可以通过 hive catalog 查询 Hive 表
-- 前提：已在 Hive 或 Spark 中创建了表（见第 3、4 节）

-- 示例：查询 Hive 创建的 employees 表（需先完成第 3.2 节）
-- SELECT * FROM hive.test_db.employees ORDER BY salary DESC;
```

### 5.4 Trino 通过 Hive catalog 建表（只读场景）

```sql
-- 以下演示 Trino 建表功能，但请注意：由于 Hive 4.0 的限制，INSERT 会失败
CREATE SCHEMA IF NOT EXISTS hive.trino_test;

CREATE TABLE hive.trino_test.products (
    id    INT,
    name  VARCHAR,
    price DOUBLE
)
WITH (format = 'TEXTFILE');

-- ⚠️ INSERT 会报错 "Cannot write to non-managed Hive table"
-- INSERT INTO hive.trino_test.products VALUES (1, 'Laptop', 1200.00);

-- 验证 Hive 可以看到这张表
-- docker exec -it hiveserver2 beeline -u "jdbc:hive2://localhost:10000" -e "SHOW TABLES IN trino_test;"
```

### 5.5 Trino 查询 MySQL

```sql
-- 查询第 2.3 节创建的业务数据
SELECT * FROM mysql.biz.users;
-- 预期: Alice, Bob, Carol

-- 查询 Hive 元数据
SELECT TBL_NAME, TBL_TYPE FROM mysql.metastore.TBLS LIMIT 5;
```

### 5.6 Trino 查询性能统计

```sql
-- 查看查询执行统计
EXPLAIN (TYPE DISTRIBUTED)
SELECT name, price FROM hive.trino_test.products WHERE price > 600;

-- 查看查询历史
SELECT query_id, query, state, elapsed
FROM system.runtime.queries
ORDER BY query_start DESC
LIMIT 5;
```

### 5.7 自定义 Plugin 加载测试

Trino Plugin 必须按 Trino 468 SPI 构建。将插件发布包解压到独立子目录，且目录中只保留 jar：

```text
extensions/trino/plugins/my-plugin/
  my-plugin.jar
  dependency-a.jar
```

Connector 对应的 Catalog 配置放在 `extensions/trino/catalogs/`。重新构建派生镜像并重建所有 Trino 节点：

```bash
docker compose build trino-coordinator
docker compose up -d --force-recreate trino-coordinator trino-worker-1

# Coordinator 和 Worker 均应出现插件加载日志
docker compose logs trino-coordinator trino-worker-1 | \
  grep -E 'Loading plugin|Installing'

# 确认两个节点均为 active，且原有 Catalog 未丢失
docker compose exec trino-coordinator trino --server http://127.0.0.1:8099 \
  --execute "SELECT node_id, coordinator, state FROM system.runtime.nodes; SHOW CATALOGS;"
```

预期：自定义插件在 Coordinator 和 Worker 各加载一次，两个节点均为 `active`，并保留 `hive`、`iceberg`、`mysql`、`system` Catalog。新增 Connector 时还应验证对应 Catalog 出现在 `SHOW CATALOGS` 中。

### 5.8 清理

```sql
DROP TABLE IF EXISTS hive.trino_test.products;
DROP SCHEMA IF EXISTS hive.trino_test;
```

---

## 6. Iceberg 跨引擎测试

这是本环境的核心测试：验证 Trino 和 Spark 能读写同一张 Iceberg 表。Iceberg 使用 Hive Metastore 作为 catalog，HDFS 作为存储层。

### 6.1 Trino 创建 Iceberg 表

```sql
CREATE SCHEMA IF NOT EXISTS iceberg.demo;

CREATE TABLE iceberg.demo.orders (
    order_id BIGINT,
    user_id  BIGINT,
    product  VARCHAR,
    amount   DOUBLE,
    ts       TIMESTAMP(6)
)
WITH (
    partitioning = ARRAY['day(ts)'],
    format = 'PARQUET'
);

-- 写入 5 条数据
INSERT INTO iceberg.demo.orders VALUES
    (1, 100, 'Widget',    29.99, TIMESTAMP '2026-07-01 10:00:00'),
    (2, 101, 'Gadget',    49.99, TIMESTAMP '2026-07-01 11:30:00'),
    (3, 100, 'Widget',    29.99, TIMESTAMP '2026-07-02 09:15:00'),
    (4, 102, 'Doohickey', 15.49, TIMESTAMP '2026-07-02 14:00:00'),
    (5, 101, 'Widget',    29.99, TIMESTAMP '2026-07-03 16:45:00');

SELECT * FROM iceberg.demo.orders ORDER BY ts;
-- 预期: 5 行，按时间排序

-- Iceberg 元数据视图
SELECT * FROM iceberg.demo."orders$snapshots";
-- 预期: 至少 1 个快照记录，记录 snapshot_id

SELECT * FROM iceberg.demo."orders$partitions";
-- 预期: 3 个分区（7/1, 7/2, 7/3），每分区行数和文件数

SELECT * FROM iceberg.demo."orders$files";
-- 预期: 数据文件列表，含路径、格式、记录数
```

### 6.2 验证 HDFS 物理文件

```bash
docker exec -it namenode hdfs dfs -ls -R /user/hive/warehouse/demo.db/orders
# 预期: data/ 目录下有 parquet 文件，metadata/ 目录下有 json 快照文件
```

### 6.3 Spark 读取 Trino 创建的表

```bash
docker exec -it spark-master /opt/spark/bin/spark-sql --master spark://spark-master:7077
```

```sql
-- 切换到 iceberg catalog
USE iceberg;

SHOW DATABASES;
-- 预期: 包含 demo

SHOW TABLES IN demo;
-- 预期: orders

SELECT * FROM demo.orders ORDER BY ts;
-- 预期: 5 行，与 Trino 写入一致

SELECT product, COUNT(*) AS cnt, SUM(amount) AS total
FROM demo.orders
GROUP BY product
ORDER BY total DESC;
-- 预期: Widget 3行 89.97, Gadget 1行 49.99, Doohickey 1行 15.49
```

### 6.4 Spark 写入 + UPDATE/DELETE

```sql
-- 写入新数据
INSERT INTO demo.orders VALUES
    (6, 200, 'Gadget', 49.99, TIMESTAMP '2026-07-04 10:00:00'),
    (7, 201, 'Widget', 29.99, TIMESTAMP '2026-07-04 11:00:00');

-- UPDATE（Iceberg ACID）
UPDATE demo.orders SET amount = 39.99 WHERE order_id = 6;

-- DELETE
DELETE FROM demo.orders WHERE order_id = 5;

SELECT count(*) FROM demo.orders;
-- 预期: 6（原5 + 新2 - 删1）

exit;
```

### 6.5 Trino 验证 Spark 的修改

```sql
-- 回到 Trino，验证 Spark 的写入和修改
SELECT * FROM iceberg.demo.orders ORDER BY order_id;

SELECT * FROM iceberg.demo.orders WHERE order_id = 6;
-- 预期: amount = 39.99（Spark UPDATE 生效）

SELECT count(*) FROM iceberg.demo.orders;
-- 预期: 6（Spark DELETE 生效）
```

### 6.6 Time Travel

```sql
-- 查看快照历史
SELECT snapshot_id, committed_at, operation, summary
FROM iceberg.demo."orders$snapshots"
ORDER BY committed_at;
-- 预期: 多个快照，operation 包含 INSERT/UPDATE/DELETE

-- 回到第一个快照（替换为实际的 snapshot_id）
-- SELECT * FROM iceberg.demo.orders FOR VERSION AS OF <first_snapshot_id>;
-- 预期: 5 行原始数据（UPDATE/DELETE 之前的版本）
```

### 6.7 Iceberg 表维护（Spark）

```bash
docker exec -it spark-master /opt/spark/bin/spark-sql --master spark://spark-master:7077
```

```sql
USE iceberg;

-- 合并小文件
CALL iceberg.system.rewrite_data_files('demo.orders');
-- 预期: rewritten_files_count > 0 或 0（视文件大小）

-- 过期旧快照（保留 2026-07-04 之后的）
CALL iceberg.system.expire_snapshots('demo.orders', TIMESTAMP '2026-07-04 00:00:00');

-- 删除孤儿文件
CALL iceberg.system.remove_orphan_files('demo.orders');

exit;
```

### 6.8 清理

```sql
-- Trino 或 Spark 均可
DROP TABLE iceberg.demo.orders;
DROP SCHEMA iceberg.demo;
```

---

## 7. 多组件组合场景

### 7.1 全链路：HDFS → Hive → Spark → Trino

验证数据从 HDFS 原始文件，经 Hive 建外部表，Spark 处理建汇总表，Trino 查询的完整链路。

**步骤 1：准备 HDFS 数据**

```bash
docker exec -it namenode bash

hdfs dfs -mkdir -p /data/logs

cat > /tmp/access.log << 'EOF'
2026-07-01,Alice,login
2026-07-01,Bob,logout
2026-07-01,Alice,view
2026-07-02,Charlie,login
2026-07-02,Alice,logout
2026-07-03,Bob,login
EOF

hdfs dfs -put /tmp/access.log /data/logs/
hdfs dfs -ls /data/logs/
# 预期: access.log

exit
```

**步骤 2：Hive 创建外部表**

```bash
docker exec -it hiveserver2 beeline -u "jdbc:hive2://localhost:10000"
```

```sql
CREATE DATABASE IF NOT EXISTS pipeline;
USE pipeline;

CREATE EXTERNAL TABLE access_logs (
    dt       STRING,
    username STRING,
    action   STRING
)
ROW FORMAT DELIMITED
FIELDS TERMINATED BY ','
LOCATION '/data/logs';

SELECT * FROM access_logs;
-- 预期: 6 行

SELECT username, COUNT(*) AS actions FROM access_logs GROUP BY username;
-- 预期: Alice 3, Bob 2, Charlie 1

!exit;
```

**步骤 3：Spark 读取 Hive 表并建汇总表**

```bash
docker exec -it spark-master /opt/spark/bin/spark-sql --master spark://spark-master:7077
```

```sql
USE pipeline;

-- CTAS 创建汇总表
CREATE TABLE daily_user_actions AS
SELECT
    dt,
    username,
    COUNT(*) AS action_count,
    COLLECT_SET(action) AS actions
FROM access_logs
GROUP BY dt, username;

SELECT * FROM daily_user_actions ORDER BY dt, username;
-- 预期:
-- 2026-07-01  Alice    2  ["login","view"]
-- 2026-07-01  Bob      1  ["logout"]
-- 2026-07-02  Alice    1  ["logout"]
-- 2026-07-02  Charlie  1  ["login"]
-- 2026-07-03  Bob      1  ["login"]

exit;
```

**步骤 4：Trino 查询 Spark 生成的表**

```bash
trino --server localhost:8099
```

```sql
SELECT * FROM hive.pipeline.daily_user_actions ORDER BY dt, action_count DESC;

-- 跨表 JOIN
SELECT
    a.username,
    a.action,
    d.action_count
FROM hive.pipeline.access_logs a
JOIN hive.pipeline.daily_user_actions d
  ON a.username = d.username AND a.dt = d.dt
ORDER BY d.action_count DESC, a.username, a.dt;
```

**步骤 5：清理**

```sql
DROP SCHEMA hive.pipeline CASCADE;
```

```bash
docker exec -it namenode hdfs dfs -rm -r /data
```

### 7.2 Iceberg 数据湖：多引擎读写 + Time Travel

**步骤 1：Spark 批量写入 Iceberg**

```bash
docker exec -it spark-master /opt/spark/bin/spark-sql --master spark://spark-master:7077
```

```sql
CREATE SCHEMA IF NOT EXISTS iceberg.lake;
USE iceberg;

CREATE TABLE lake.events (
    event_id   BIGINT,
    event_type VARCHAR,
    user_id    BIGINT,
    ts         TIMESTAMP
) USING iceberg PARTITIONED BY (days(ts));

INSERT INTO lake.events VALUES
    (1, 'click',    100, TIMESTAMP '2026-07-01 10:00:00'),
    (2, 'view',     101, TIMESTAMP '2026-07-01 10:05:00'),
    (3, 'click',    100, TIMESTAMP '2026-07-01 10:10:00'),
    (4, 'purchase', 102, TIMESTAMP '2026-07-01 11:00:00'),
    (5, 'view',     101, TIMESTAMP '2026-07-02 09:00:00');

exit;
```

**步骤 2：Trino 交互式分析 + 增量写入**

```sql
SELECT event_type, COUNT(*) AS cnt
FROM iceberg.lake.events
GROUP BY event_type
ORDER BY cnt DESC;
-- 预期: click 2, view 2, purchase 1

-- MERGE 写入（Trino 468 支持）
MERGE INTO iceberg.lake.events e
USING (VALUES (6, 'click', 103, TIMESTAMP '2026-07-03 08:00:00'))
  AS new(id, type, uid, ts)
  ON e.event_id = new.id
WHEN NOT MATCHED THEN INSERT VALUES (new.id, new.type, new.uid, new.ts);

SELECT count(*) FROM iceberg.lake.events;
-- 预期: 6
```

**步骤 3：Spark 验证 + Time Travel**

```bash
docker exec -it spark-master /opt/spark/bin/spark-sql --master spark://spark-master:7077
```

```sql
USE iceberg;

SELECT count(*) FROM lake.events;
-- 预期: 6

-- Iceberg 元数据表（Spark 语法）
SELECT * FROM lake.events.history;
-- 预期: 多个版本记录

SELECT * FROM lake.events.snapshots;
-- 预期: 快照列表，含 snapshot_id 和 timestamp

SELECT * FROM lake.events.files;
-- 预期: 数据文件列表

exit;
```

**步骤 4：清理**

```sql
-- Trino 或 Spark
DROP TABLE iceberg.lake.events;
DROP SCHEMA iceberg.lake;
```

### 7.3 Trino 联邦查询

验证 Trino 同时查询 MySQL 和 Hive，做跨数据源 JOIN。

> 前置：已完成 [2.3 节](#23-创建业务数据供-trino-联邦查询使用)创建 `mysql.biz.users` 表。

**准备 Hive 侧数据：**

```sql
-- 在 Trino 中创建 Hive 订单表并关联 MySQL 的用户 ID
CREATE SCHEMA IF NOT EXISTS hive.fed;

CREATE TABLE hive.fed.orders (
    order_id BIGINT,
    user_id  BIGINT,
    product  VARCHAR,
    amount   DOUBLE
)
WITH (format = 'TEXTFILE');

INSERT INTO hive.fed.orders VALUES
    (1, 100, 'Laptop',  1200.00),
    (2, 101, 'Phone',    800.00),
    (3, 100, 'Mouse',     25.00),
    (4, 102, 'Keyboard',  75.00);
```

**跨数据源 JOIN：**

```sql
-- Trino 同时查 MySQL（用户信息）和 Hive（订单数据）
SELECT
    u.name  AS user_name,
    u.dept,
    o.product,
    o.amount
FROM mysql.biz.users u
JOIN hive.fed.orders o ON u.id = o.user_id
ORDER BY o.amount DESC;
-- 预期:
-- Alice   Engineering  Laptop    1200.00
-- Bob     Sales        Phone      800.00
-- Carol   Marketing    Keyboard    75.00
-- Alice   Engineering  Mouse       25.00

-- 聚合：按部门统计消费
SELECT
    u.dept,
    COUNT(*)    AS order_count,
    SUM(o.amount) AS total_spent
FROM mysql.biz.users u
JOIN hive.fed.orders o ON u.id = o.user_id
GROUP BY u.dept
ORDER BY total_spent DESC;
-- 预期:
-- Engineering  2  1225.00
-- Sales        1   800.00
-- Marketing    1    75.00
```

**清理：**

```sql
DROP TABLE hive.fed.orders;
DROP SCHEMA hive.fed;
```

```sql
-- 如需清理 MySQL 业务数据
-- DROP DATABASE biz;
```

### 7.4 Spark DataFrame API → Iceberg → Trino

验证通过 Spark DataFrame API 写入 Iceberg 表，再用 Trino 读取。

> 前置：需确保 `iceberg.demo` schema 已存在。如第 6 节已清理，需先执行：
> `docker compose exec -T trino-coordinator trino --server http://127.0.0.1:8099 --execute "CREATE SCHEMA IF NOT EXISTS iceberg.demo"`

```bash
docker exec -it spark-master /opt/spark/bin/spark-shell --master spark://spark-master:7077
```

```scala
import org.apache.spark.sql.SaveMode

// 创建 DataFrame
val metrics = Seq(
  ("2026-07-01", "cpu",     85.5),
  ("2026-07-01", "memory",  72.3),
  ("2026-07-02", "cpu",     91.2),
  ("2026-07-02", "memory",  68.0)
).toDF("dt", "metric", "value")

// 写入 Iceberg 表（通过 iceberg catalog）
metrics.writeTo("iceberg.demo.metrics").createOrReplace()

// 验证写入
spark.table("iceberg.demo.metrics").show()
// 预期: 4 行

:quit
```

回到 Trino 验证：

```sql
SELECT * FROM iceberg.demo.metrics ORDER BY dt, metric;
-- 预期: 4 行，与 Spark 写入一致

SELECT metric, AVG(value) AS avg_value
FROM iceberg.demo.metrics
GROUP BY metric;
-- 预期: cpu 88.35, memory 70.15
```

清理：

```sql
DROP TABLE iceberg.demo.metrics;
```

### 7.5 Hive 分区表跨引擎访问

验证 Hive 分区表能被 Spark 和 Trino 正确访问。

**步骤 1：Hive 创建分区表**

```bash
docker exec -it hiveserver2 beeline -u "jdbc:hive2://localhost:10000"
```

```sql
CREATE DATABASE IF NOT EXISTS part_test;
USE part_test;

CREATE TABLE logs (
    id     INT,
    msg    STRING
)
PARTITIONED BY (dt STRING)
ROW FORMAT DELIMITED
FIELDS TERMINATED BY ',';

-- 插入分区数据
INSERT INTO logs PARTITION (dt='2026-07-01') VALUES (1, 'morning'), (2, 'noon');
INSERT INTO logs PARTITION (dt='2026-07-02') VALUES (3, 'evening');

-- 查看分区
SHOW PARTITIONS logs;
-- 预期: dt=2026-07-01, dt=2026-07-02

SELECT * FROM logs WHERE dt = '2026-07-01';
-- 预期: 2 行

!exit;
```

**步骤 2：Spark 读取分区表**

```bash
docker exec -it spark-master /opt/spark/bin/spark-sql --master spark://spark-master:7077
```

```sql
USE part_test;

SHOW PARTITIONS logs;
-- 预期: dt=2026-07-01, dt=2026-07-02

SELECT * FROM logs ORDER BY id;
-- 预期: 3 行

-- 按分区过滤
SELECT count(*) FROM logs WHERE dt = '2026-07-02';
-- 预期: 1

exit;
```

**步骤 3：Trino 读取分区表**

```sql
SELECT * FROM hive.part_test.logs ORDER BY id;
-- 预期: 3 行

SELECT dt, count(*) AS cnt
FROM hive.part_test.logs
GROUP BY dt
ORDER BY dt;
-- 预期: 2026-07-01 2, 2026-07-02 1
```

**步骤 4：清理**

```sql
-- Trino 或 Hive
DROP SCHEMA hive.part_test CASCADE;
```

---

## 附录：排障命令与常见问题

### 查看日志

```bash
# 某服务实时日志
docker compose logs -f hivemetastore

# 最后 100 行
docker compose logs --tail 100 spark-master

# Trino 日志（镜像版本间容器内日志路径可能不同，优先使用 Compose）
docker compose logs --tail 100 trino-coordinator
```

### 查看容器资源

```bash
docker stats --no-stream \
  namenode datanode mysql hivemetastore hiveserver2 \
  spark-master spark-worker trino-coordinator trino-worker-1
```

### HDFS 块报告

```bash
docker exec -it namenode hdfs fsck / -files -blocks
```

### 重启某服务

```bash
docker compose restart hivemetastore
docker compose restart trino-coordinator trino-worker-1
```

### 完全重置（清除所有数据）

```bash
docker compose down -v    # -v 删除所有 volume
./bootstrap.sh
```

### 常见问题

| 问题                      | 原因                                | 解决                                                                  |
| ------------------------- | ----------------------------------- | --------------------------------------------------------------------- |
| Trino 连接被拒            | 服务未就绪                          | 等待 healthcheck 通过，或 `docker compose restart trino-coordinator`  |
| Hive 建表失败             | Metastore 未就绪                    | `docker compose logs hivemetastore` 检查                              |
| Spark 看不到 Hive 表      | hive-site.xml 未挂载                | 确认 `docker-compose.yml` volumes 配置                                |
| Trino 看不到 Iceberg 表   | catalog 配置错误                    | 检查 `extensions/trino/catalogs/iceberg.properties`                   |
| Trino 无法写入 Hive 表    | Hive 4.0 默认建表为 EXTERNAL_TABLE  | Trino Hive connector 不允许写入外部表；如需写入请使用 Iceberg catalog |
| Trino 查 MySQL 报表不存在 | `hive` 用户无权访问 `root` 创建的库 | 在 MySQL 中执行 `GRANT ALL PRIVILEGES ON biz.* TO 'hive'@'%'`         |
| Spark 看不到 Iceberg 表   | iceberg jar 未下载                  | 运行 `./bootstrap.sh` 重新下载                                        |
| HDFS 空间不足             | DataNode 容量限制                   | 清理无用数据或调整 `deploy.resources`                                 |
| 端口冲突                  | 宿主机端口被占用                    | 修改 `docker-compose.yml` 端口映射                                    |
| 首次启动 Trino 失败       | metastore 还在初始化                | 等待 1-2 分钟后 `docker compose restart`                              |
