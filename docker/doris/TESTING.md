# Doris 模块测试指南

本文档覆盖 Doris 集群（1 FE + 2 BE）的功能验证，包括建表模型、数据导入、查询能力、高可用与 Schema 变更。

> **前置条件**：已通过 `./bootstrap.sh` 启动集群，且 `SHOW BACKENDS\G` 显示 2 个 BE 均 `Alive: true`。

> **约定**：各章节自包含（自带建表和清理），可独立执行；`demo.orders`（`init-sql/01-init.sql` 自动创建）除外，作为常驻示例表保留，不建议删除。执行命令前先加载密码：`export DORIS_ROOT_PASSWORD=$(grep '^DORIS_ROOT_PASSWORD=' .env | cut -d= -f2-)`。

## 目录

- [0. 环境准备与连通性检查](#0-环境准备与连通性检查)
- [1. 建表模型测试](#1-建表模型测试)
- [2. Stream Load 导入测试](#2-stream-load-导入测试)
- [3. 查询与物化视图测试](#3-查询与物化视图测试)
- [4. 高可用与多副本测试](#4-高可用与多副本测试)
- [5. Schema Change 测试](#5-schema-change-测试)
- [附录：排障命令与常见问题](#附录排障命令与常见问题)

---

## 0. 环境准备与连通性检查

### 0.1 确认服务健康

```bash
cd doris
docker compose ps
```

预期：`doris-fe`、`doris-be1`、`doris-be2` 均为 `Up`（`doris-fe` 还会显示 `healthy`）。

### 0.2 端口连通性检查

```bash
# FE HTTP（Web 控制台 / API）
curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8030/api/bootstrap
# 预期: 200

# FE MySQL 协议端口
nc -z localhost 9030 && echo "OK"
# 预期: OK

# BE1 / BE2 Web 端口
curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8040
curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8041
# 预期: 200
```

### 0.3 连接信息速查

| 组件      | 连接方式                                | 认证                                   |
| --------- | --------------------------------------- | -------------------------------------- |
| FE MySQL  | `mysql -h 127.0.0.1 -P 9030 -u root -p` | 密码见 `.env` 的 `DORIS_ROOT_PASSWORD` |
| FE 控制台 | http://localhost:8030                   | 部分接口无需认证，默认仅绑定本机       |
| BE1 Web   | http://localhost:8040                   | 无，默认仅绑定本机                     |
| BE2 Web   | http://localhost:8041                   | 无，默认仅绑定本机                     |

### 0.4 集群状态检查

```bash
MYSQL_PWD="$DORIS_ROOT_PASSWORD" mysql -h 127.0.0.1 -P 9030 -u root
```

```sql
-- FE 节点状态（本环境仅 1 个 FE，Role 应为 FOLLOWER 且 IsMaster 为 true）
SHOW FRONTENDS\G
-- 预期: Alive = true, IsMaster = true

-- BE 节点状态
SHOW BACKENDS\G
-- 预期: 2 条记录，Alive = true，TabletNum/DataUsedCapacity 随数据量增长

-- 当前 Doris 版本
SHOW VARIABLES LIKE 'version_comment';
```

---

## 1. 建表模型测试

Doris 支持三种核心建表模型，语义差异很大，是最值得验证的部分。本节均使用独立的 `test_model` 库，互不影响，最后统一清理。

```sql
CREATE DATABASE IF NOT EXISTS test_model;
USE test_model;
```

### 1.1 Duplicate Key 模型（明细模型）

不做任何聚合，等价于普通明细表，`demo.orders` 就是这种模型。

```sql
CREATE TABLE dup_events (
    event_id   BIGINT,
    user_id    BIGINT,
    event_type VARCHAR(20),
    event_time DATETIME
)
DUPLICATE KEY(event_id)
DISTRIBUTED BY HASH(event_id) BUCKETS 4
PROPERTIES ("replication_allocation" = "tag.location.default: 2");

INSERT INTO dup_events VALUES
    (1, 100, 'click', '2026-07-01 10:00:00'),
    (2, 100, 'click', '2026-07-01 10:00:00'),  -- 与上一行完全相同
    (3, 101, 'view',  '2026-07-01 10:05:00');

SELECT * FROM dup_events ORDER BY event_id;
-- 预期: 3 行，重复行 (1) 和 (2) 都保留，不做去重/合并
```

### 1.2 Aggregate Key 模型（聚合模型）

相同 Key 的行会按列定义的聚合函数自动合并，常用于预聚合报表。

```sql
CREATE TABLE agg_pv (
    dt        DATE,
    page      VARCHAR(50),
    pv        BIGINT SUM,
    last_time DATETIME REPLACE
)
AGGREGATE KEY(dt, page)
DISTRIBUTED BY HASH(dt) BUCKETS 4
PROPERTIES ("replication_allocation" = "tag.location.default: 2");

-- 同一 (dt, page) 多次写入，pv 会 SUM 累加，last_time 会 REPLACE 为最后写入的值
INSERT INTO agg_pv VALUES ('2026-07-01', 'home', 10, '2026-07-01 09:00:00');
INSERT INTO agg_pv VALUES ('2026-07-01', 'home', 5,  '2026-07-01 10:00:00');
INSERT INTO agg_pv VALUES ('2026-07-01', 'list', 3,  '2026-07-01 09:30:00');

SELECT dt, page, SUM(pv) AS pv, MAX(last_time) AS last_time
FROM agg_pv
GROUP BY dt, page
ORDER BY page;
-- 预期: home 累计 pv=15，last_time=10:00:00；list pv=3

-- Aggregate 模型即使不加 GROUP BY，相同 Key 的行也已在存储引擎层合并
SELECT * FROM agg_pv ORDER BY page;
-- 预期: 2 行（home、list 各一行），home 的 pv 已经是合并后的 15
-- 注：小数据量通常在写入时的 MemTable 阶段就完成合并；大数据量、多批次写入时
-- 合并可能延迟到后台 compaction 阶段，此时 SELECT * 也可能短暂看到多行未合并的中间状态，
-- 用 GROUP BY 重新聚合可以保证结果始终正确
```

### 1.3 Unique Key 模型（唯一键模型）

按 Key 幂等覆盖（upsert 语义），适合维表 / 状态表。

```sql
CREATE TABLE uniq_user_status (
    user_id     BIGINT,
    user_name   VARCHAR(50),
    status      VARCHAR(20),
    update_time DATETIME
)
UNIQUE KEY(user_id)
DISTRIBUTED BY HASH(user_id) BUCKETS 4
PROPERTIES (
    "replication_allocation" = "tag.location.default: 2",
    "enable_unique_key_merge_on_write" = "true"
);

INSERT INTO uniq_user_status VALUES (1, 'Alice', 'ACTIVE', '2026-07-01 10:00:00');
INSERT INTO uniq_user_status VALUES (2, 'Bob',   'ACTIVE', '2026-07-01 10:00:00');

SELECT * FROM uniq_user_status ORDER BY user_id;
-- 预期: 2 行

-- 相同 user_id=1 再次写入，整行被覆盖（upsert）
INSERT INTO uniq_user_status VALUES (1, 'Alice', 'INACTIVE', '2026-07-02 08:00:00');

SELECT * FROM uniq_user_status ORDER BY user_id;
-- 预期: 仍 2 行，user_id=1 的 status 变为 INACTIVE，update_time 更新

-- Merge-on-Write 模式下 UPDATE / DELETE 语法可直接使用
UPDATE uniq_user_status SET status = 'BANNED' WHERE user_id = 2;
DELETE FROM uniq_user_status WHERE user_id = 1;

SELECT * FROM uniq_user_status;
-- 预期: 仅剩 user_id=2，status=BANNED
```

### 1.4 分区表 + 动态分区

```sql
CREATE TABLE part_logs (
    log_id   BIGINT,
    log_time DATETIME,
    message  VARCHAR(200)
)
DUPLICATE KEY(log_id)
PARTITION BY RANGE(log_time) (
    PARTITION p20260701 VALUES [('2026-07-01 00:00:00'), ('2026-07-02 00:00:00')),
    PARTITION p20260702 VALUES [('2026-07-02 00:00:00'), ('2026-07-03 00:00:00'))
)
DISTRIBUTED BY HASH(log_id) BUCKETS 2
PROPERTIES ("replication_allocation" = "tag.location.default: 2");

INSERT INTO part_logs VALUES
    (1, '2026-07-01 10:00:00', 'start'),
    (2, '2026-07-02 10:00:00', 'stop');

-- 查看分区信息
SHOW PARTITIONS FROM part_logs;
-- 预期: p20260701、p20260702 两个分区，各含 1 行

-- 分区裁剪：只扫描命中的分区（可用 EXPLAIN 验证）
EXPLAIN SELECT * FROM part_logs WHERE log_time >= '2026-07-02 00:00:00';
-- 预期: 执行计划中 partitions 仅命中 p20260702

-- 动态分区示例（按天自动滚动创建未来分区、清理过期分区）
CREATE TABLE dynamic_logs (
    log_id   BIGINT,
    log_time DATETIME,
    message  VARCHAR(200)
)
DUPLICATE KEY(log_id)
PARTITION BY RANGE(log_time) ()
DISTRIBUTED BY HASH(log_id) BUCKETS 2
PROPERTIES (
    "replication_allocation" = "tag.location.default: 2",
    "dynamic_partition.enable" = "true",
    "dynamic_partition.time_unit" = "DAY",
    "dynamic_partition.start" = "-3",
    "dynamic_partition.end" = "3",
    "dynamic_partition.prefix" = "p",
    "dynamic_partition.buckets" = "2"
);

-- 预期: 建表后几秒内 FE 后台调度自动创建 today-3 ~ today+3 共 7 个分区
SHOW PARTITIONS FROM dynamic_logs;
```

### 1.5 验证分桶与副本分布

```sql
-- 查看 dup_events 表的 tablet 明细，每个 tablet 应有 2 条记录（对应 2 个副本），分布在不同 BackendId 上
SHOW TABLETS FROM dup_events;

-- 更直观：按 BackendId 汇总副本数量分布
ADMIN SHOW REPLICA DISTRIBUTION FROM dup_events;
-- 预期: 2 个 BackendId，各占约 50% ReplicaNum（4 bucket × 2 副本 = 8，均分在 2 个 BE 上各 4 个）
```

### 1.6 清理

```sql
DROP DATABASE test_model FORCE;
```

---

## 2. Stream Load 导入测试

Stream Load 是 Doris 最常用的实时导入方式，通过 HTTP PUT 提交数据，同步返回导入结果。

> **两个注意事项**：
>
> 1. curl 必须显式带上 `-H "Expect:100-continue"`，否则会报 `There is no 100-continue header` 而失败。
> 2. 请求发给 FE（8030）会被 `307` 重定向到某个 BE 的**容器内网地址**（如 `172.28.10.11:8040`），宿主机默认路由不到这个地址会导致 `Couldn't connect to server`。本环境下请直接对 BE 的宿主机映射端口（`8040` 对应 be1，`8041` 对应 be2）发起请求，无需 `--location-trusted`。以下示例使用 be1；若 be1 不可用，可将端口改为 `8041`。

### 2.1 准备目标表

```bash
MYSQL_PWD="$DORIS_ROOT_PASSWORD" mysql -h 127.0.0.1 -P 9030 -u root -e "
CREATE DATABASE IF NOT EXISTS test_load;
CREATE TABLE test_load.csv_users (
    id   INT,
    name VARCHAR(50),
    age  INT
)
DUPLICATE KEY(id)
DISTRIBUTED BY HASH(id) BUCKETS 2
PROPERTIES ('replication_allocation' = 'tag.location.default: 2');
"
```

### 2.2 CSV 格式导入

```bash
cat > /tmp/users.csv << 'EOF'
1,Alice,28
2,Bob,35
3,Charlie,42
EOF

curl -v -u "root:${DORIS_ROOT_PASSWORD}" \
    -H "Expect:100-continue" \
    -H "label:csv_users_$(date +%s)" \
    -H "column_separator:," \
    -H "columns: id, name, age" \
    -T /tmp/users.csv \
    -XPUT http://localhost:8040/api/test_load/csv_users/_stream_load
```

预期响应 JSON 中 `"Status": "Success"`，`NumberLoadedRows` 为 3。

```sql
SELECT * FROM test_load.csv_users ORDER BY id;
-- 预期: 3 行
```

### 2.3 JSON 格式导入

```bash
cat > /tmp/users.json << 'EOF'
[
  {"id": 4, "name": "Diana", "age": 31},
  {"id": 5, "name": "Eve",   "age": 26}
]
EOF

curl -v -u "root:${DORIS_ROOT_PASSWORD}" \
    -H "Expect:100-continue" \
    -H "label:json_users_$(date +%s)" \
    -H "format:json" \
    -H "strip_outer_array:true" \
    -T /tmp/users.json \
    -XPUT http://localhost:8040/api/test_load/csv_users/_stream_load
```

```sql
SELECT * FROM test_load.csv_users ORDER BY id;
-- 预期: 5 行（原 3 行 + 新增 2 行）
```

### 2.4 查看导入历史与状态

```sql
-- FE 侧记录的导入任务（Stream Load 属于同步导入，也会留痕）
SHOW LOAD FROM test_load ORDER BY CreateTime DESC LIMIT 5\G
```

```bash
# 也可以直接看 curl 返回的 JSON，关注以下字段：
#   "Status"           : Success / Fail
#   "NumberLoadedRows"  : 实际导入行数
#   "NumberFilteredRows": 被过滤（脏数据）行数
#   "LoadBytes"         : 导入字节数
```

### 2.5 错误数据与容忍度

默认（非严格模式）下，Doris 对类型不匹配会做宽松处理（例如字符串截断、非法数字转 `NULL`）而不是直接过滤丢弃整行。要让不合法的数据被真正过滤统计，需要显式开启 `strict_mode`：

```bash
# 建一张 name 只有 5 个字符的表，便于演示宽松模式下的自动截断
MYSQL_PWD="$DORIS_ROOT_PASSWORD" mysql -h 127.0.0.1 -P 9030 -u root -e "
  CREATE TABLE test_load.strict_users (id INT, name VARCHAR(5), age INT)
  DUPLICATE KEY(id) DISTRIBUTED BY HASH(id) BUCKETS 2
  PROPERTIES ('replication_allocation' = 'tag.location.default: 2');
"

cat > /tmp/bad_users.csv << 'EOF'
6,ThisNameIsWayTooLong,40
7,Grace,29
EOF

# 非严格模式（默认）：超长字符串被静默截断，不会过滤
curl -s -u "root:${DORIS_ROOT_PASSWORD}" \
    -H "Expect:100-continue" \
    -H "label:bad_users_$(date +%s)" \
    -H "column_separator:," \
    -H "columns: id, name, age" \
    -T /tmp/bad_users.csv \
    -XPUT http://localhost:8040/api/test_load/strict_users/_stream_load
# 预期: Status=Success, NumberFilteredRows=0

MYSQL_PWD="$DORIS_ROOT_PASSWORD" mysql -h 127.0.0.1 -P 9030 -u root -e "SELECT * FROM test_load.strict_users"
# 预期: id=6 的 name 被截断为 'ThisN'
```

```bash
cat > /tmp/bad_users2.csv << 'EOF'
8,Henry,abc
9,Ivy,29
EOF

# strict_mode:true + max_filter_ratio 允许一定比例的脏数据被过滤而不整体失败
curl -s -u "root:${DORIS_ROOT_PASSWORD}" \
    -H "Expect:100-continue" \
    -H "label:bad_users2_$(date +%s)" \
    -H "column_separator:," \
    -H "columns: id, name, age" \
    -H "strict_mode:true" \
    -H "max_filter_ratio:0.5" \
    -T /tmp/bad_users2.csv \
    -XPUT http://localhost:8040/api/test_load/strict_users/_stream_load
# 预期: Status=Success（0.5 的容忍度覆盖了 1/2 的脏行），NumberFilteredRows=1
# 响应中的 ErrorURL 字段可直接 curl 查看具体哪一行、哪个字段转换失败

MYSQL_PWD="$DORIS_ROOT_PASSWORD" mysql -h 127.0.0.1 -P 9030 -u root -e "SELECT * FROM test_load.strict_users WHERE id IN (8, 9)"
-- 预期: 仅 id=9 (Ivy) 被导入，id=8 因 age='abc' 无法转换为 INT 被过滤
```

### 2.6 清理

```sql
DROP DATABASE test_load FORCE;
```

---

## 3. 查询与物化视图测试

### 3.1 基础聚合与 JOIN（使用 demo.orders）

```sql
USE demo;

SELECT status, COUNT(*) AS cnt, SUM(amount) AS total
FROM orders
GROUP BY status
ORDER BY total DESC;
-- 预期: PAID 3 单 276.40，PENDING 1 单 12.00，CANCELLED 1 单 9.90

-- 自关联示例：找出每个用户金额最高的一笔订单
SELECT o.user_id, o.order_id, o.amount
FROM orders o
JOIN (
    SELECT user_id, MAX(amount) AS max_amount
    FROM orders
    GROUP BY user_id
) m ON o.user_id = m.user_id AND o.amount = m.max_amount
ORDER BY o.user_id;
```

### 3.2 EXPLAIN 查看 MPP 执行计划

```sql
EXPLAIN SELECT status, SUM(amount) FROM orders GROUP BY status;
-- 预期: 出现 PLAN FRAGMENT 0 / 1，体现 Doris 的分片并行聚合（先局部聚合，FE 汇总）
```

### 3.3 物化视图（同步物化视图）

物化视图能针对高频聚合查询自动预计算，Doris 优化器会自动路由命中，无需改写 SQL。使用时有两条容易踩坑的限制：

- **SELECT 列表中的列名不能与原表列名重复**（包括作为 GROUP BY key 的列），必须用 `AS 别名` 规避，否则建表报 `Duplicate column name`
- 同步物化视图**不能被直接 SELECT**，只能通过查询原表、由优化器自动改写命中

```sql
-- 基于 orders 创建按 status 汇总金额的物化视图
-- 注意 status 必须起别名（如 status_），否则与 orders 表自身的 status 列冲突而建表失败
CREATE MATERIALIZED VIEW mv_orders_by_status AS
SELECT status AS status_, SUM(amount) AS total_amount, COUNT(*) AS cnt
FROM orders
GROUP BY status;

-- 物化视图构建是异步任务，轮询直到 State = FINISHED
SHOW ALTER TABLE MATERIALIZED VIEW FROM demo\G

-- 构建完成后，查询原表时优化器会自动改写命中物化视图（无需、也不能直接查 mv_orders_by_status）
EXPLAIN SELECT status, SUM(amount) FROM orders GROUP BY status;
-- 预期: 执行计划的 TABLE 一行显示 demo.orders(mv_orders_by_status)，
-- 且末尾 MATERIALIZATIONS 部分的 MaterializedViewRewriteSuccessAndChose 命中该视图

SELECT status, SUM(amount) FROM orders GROUP BY status ORDER BY status;
-- 预期: 结果与 3.1 一致（CANCELLED 9.90 / PAID 276.40 / PENDING 12.00），但扫描的是预聚合数据
```

清理：

```sql
DROP MATERIALIZED VIEW mv_orders_by_status ON orders;
```

---

## 4. 高可用与多副本测试

`demo.orders` 及 [第 1 节](#1-建表模型测试) 中的表均设置 `replication_allocation = tag.location.default: 2`，即每个 tablet 在 2 个 BE 上都有副本。本节验证停掉一个 BE 后，**查询**是否还能正常返回，以及**写入**在副本不齐时的真实表现（结论和直觉不完全一致，见 4.3 节）。

### 4.1 确认当前副本分布

```sql
-- 查看 orders 表各 tablet 的副本落在哪些 BE 上
SHOW TABLETS FROM demo.orders;

-- 更直观的副本分布统计
ADMIN SHOW REPLICA DISTRIBUTION FROM demo.orders;
-- 预期: 2 个 BackendId，各占约 50% ReplicaNum
```

### 4.2 停掉一个 BE，验证查询不受影响

```bash
# 停掉 be1（对应 docker-compose.yml 中的 172.28.10.11）
docker compose stop be1

# 稍等几秒后查看集群状态
MYSQL_PWD="$DORIS_ROOT_PASSWORD" mysql -h 127.0.0.1 -P 9030 -u root -e "SHOW BACKENDS\G" | grep -E "Host|Alive"
# 预期: Host 172.28.10.11（be1）对应 Alive = false，172.28.10.12（be2）仍为 true
```

```sql
-- 数据依然可查（另一副本还在 be2 上），可能会有短暂的 fail-over 延迟
SELECT COUNT(*) FROM demo.orders;
-- 预期: 5（与停机前一致）

SELECT * FROM demo.orders ORDER BY order_id;
-- 预期: 完整 5 行数据，无缺失
```

### 4.3 验证单 BE 存活时的写入行为（会失败，这是预期行为）

```sql
INSERT INTO demo.orders (order_id, user_id, amount, order_time, status)
VALUES (99, 1099, 66.60, '2026-07-06 12:00:00', 'PAID');
-- 预期: 报错，类似
-- ERROR 1105 (HY000): errCode = 2, detailMessage = errCode = 3, detailMessage = tablet ... alive replica
-- num 1 < load required replica num 2, alive backends: [...]
```

> **关键结论**：`replication_allocation=2` 只保证了**读高可用**（缺一个副本时查询仍可用旧副本正常返回），但**不保证写高可用**——Doris 默认要求写入时满足多数派/全部副本存活（quorum），只要有 tablet 的存活副本数低于配置的副本数，写入就会直接失败，而不是"降级写入、待节点恢复后自动补齐"。这是很多初次接触 Doris 的人容易搞错的地方；如果业务要求"少数副本故障时仍可写入"，需要把副本数设置为 3 及以上（多数派容忍 1 台故障），2 副本配置本质上只能容忍"只读降级"。

### 4.4 恢复 BE，验证副本重新可写

```bash
docker compose start be1

# 等待 be1 重新注册
for i in $(seq 1 30); do
    ALIVE=$(MYSQL_PWD="$DORIS_ROOT_PASSWORD" mysql -h 127.0.0.1 -P 9030 -u root -N -e "SHOW BACKENDS" 2>/dev/null | awk -F'\t' '{for(i=1;i<=NF;i++) if($i=="true") c++} END{print c+0}')
    [ "${ALIVE:-0}" -ge 2 ] && break
    sleep 2
done
echo "存活 BE 数: $ALIVE"
```

```sql
-- 2 个 BE 都恢复存活后，写入应恢复正常
INSERT INTO demo.orders (order_id, user_id, amount, order_time, status)
VALUES (99, 1099, 66.60, '2026-07-06 12:00:00', 'PAID');

SELECT * FROM demo.orders WHERE order_id = 99;
-- 预期: 正常返回

ADMIN SHOW REPLICA DISTRIBUTION FROM demo.orders;
-- 预期: 两个 BackendId 副本数重新均衡
```

### 4.5 清理

```sql
DELETE FROM demo.orders WHERE order_id = 99;
```

> 若想验证"两个 BE 都不可用"的极端场景，预期是查询和写入均直接报错（无可用副本）。这也说明 `replication_allocation` 设置为几并非越大越好而是按需权衡：副本数越高，可容忍的同时故障数越多，但存储和写入开销也越大。

---

## 5. Schema Change 测试

Doris 的加列、修改类型等 DDL 是异步执行的（后台重写数据），语法上和同步 DDL 一样，但需要轮询状态确认完成。

### 5.1 加列

```sql
USE demo;

ALTER TABLE orders ADD COLUMN remark VARCHAR(100) DEFAULT '';

-- 异步任务，轮询直到 State = FINISHED
SHOW ALTER TABLE COLUMN FROM demo ORDER BY CreateTime DESC LIMIT 1\G

DESC orders;
-- 预期: 新增 remark 列

SELECT order_id, remark FROM orders LIMIT 3;
-- 预期: remark 均为空字符串（默认值）
```

### 5.2 修改列类型

```sql
-- amount 从 DECIMAL(10,2) 扩大精度为 DECIMAL(12,2)
ALTER TABLE orders MODIFY COLUMN amount DECIMAL(12, 2);

SHOW ALTER TABLE COLUMN FROM demo ORDER BY CreateTime DESC LIMIT 1\G
-- 预期: 等待 State 变为 FINISHED

DESC orders;
-- 预期: amount 类型变为 decimal(12,2)
```

### 5.3 还原本节变更

```sql
ALTER TABLE orders DROP COLUMN remark;
-- 等待 DROP COLUMN 任务变为 FINISHED 后，再执行下一条 ALTER
SHOW ALTER TABLE COLUMN FROM demo ORDER BY CreateTime DESC LIMIT 1\G

ALTER TABLE orders MODIFY COLUMN amount DECIMAL(10, 2);
-- 等待类型还原任务变为 FINISHED
SHOW ALTER TABLE COLUMN FROM demo ORDER BY CreateTime DESC LIMIT 1\G

DESC orders;
-- 预期: remark 列已移除，amount 类型已还原为 decimal(10,2)
```

---

## 附录：排障命令与常见问题

### 查看日志

```bash
# FE 日志
docker compose logs -f fe

# BE 日志
docker compose logs --tail 200 be1

# 容器内日志文件（更详细，包含 GC 日志等）
docker exec -it doris-fe tail -f /opt/apache-doris/fe/log/fe.log
docker exec -it doris-be1 tail -f /opt/apache-doris/be/log/be.log
```

### 查看容器资源

```bash
docker stats --no-stream doris-fe doris-be1 doris-be2
```

### 手动检查集群与任务状态

```sql
-- 集群整体状态
SHOW FRONTENDS\G
SHOW BACKENDS\G

-- 正在运行/排队的导入、Schema Change、物化视图任务（均需指定具体数据库，不能省略 FROM）
SHOW LOAD FROM demo ORDER BY CreateTime DESC LIMIT 10;
SHOW ALTER TABLE COLUMN FROM demo ORDER BY CreateTime DESC LIMIT 10;
SHOW ALTER TABLE MATERIALIZED VIEW FROM demo ORDER BY CreateTime DESC LIMIT 10;

-- 正在执行的 SQL（排查慢查询/卡住的连接）
SHOW PROCESSLIST;
```

### 重启某服务

```bash
docker compose restart be1
docker compose restart fe   # 谨慎：FE 重启期间集群短暂不可写
```

### 完全重置（清除所有数据和本模块 `.env`）

```bash
./bootstrap.sh --reset
```

该命令会删除命名卷和 `.env`，随后重新生成随机密码并启动集群。若希望保留现有密码，只清除数据卷，可执行 `docker compose down -v && ./bootstrap.sh`。

### 常见问题

| 问题                                                             | 原因                                                                      | 解决                                                                                                              |
| ---------------------------------------------------------------- | ------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------- |
| BE 一直不 Alive                                                  | FE/BE 之间网络或心跳端口不通                                              | 检查 `docker compose logs be1`，确认 `FE_SERVERS`/`BE_ADDR` 与网络配置一致                                        |
| Stream Load 报 `Fail` + 找不到表                                 | 表未创建或库名/表名拼写错误                                               | 先 `SHOW TABLES FROM <db>` 确认表存在                                                                             |
| Stream Load 报 `There is no 100-continue header`                 | curl 未带 `Expect` 头                                                     | 加上 `-H "Expect:100-continue"`                                                                                   |
| Stream Load 请求 FE 后报 `Couldn't connect to server`            | FE 把请求 307 重定向到了 BE 的容器内网地址，宿主机路由不到                | 直接对 BE 的宿主机映射端口（`8040`/`8041`）发起请求，无需经过 FE                                                  |
| 建表报 `Failed to find enough backend`                           | 可用 BE 数量小于副本数                                                    | 确认 2 个 BE 均 Alive，或调低 `replication_allocation`                                                            |
| Schema Change 长时间 PENDING                                     | 数据量大导致后台重写耗时长                                                | `SHOW ALTER TABLE COLUMN` 观察进度，或检查 BE 是否有资源瓶颈                                                      |
| 聚合模型 `SELECT *` 看到未合并的多行                             | Compaction 尚未执行，数据还在 base/cumulative rowset 中                   | 属正常现象，可用 `GROUP BY` 主动聚合，或等待后台 compaction                                                       |
| 停 BE 后查询报错 `no queryable replica`                          | 该 tablet 的另一副本也不可用，或副本数设置为 1                            | 确认 `replication_allocation` ≥ 2，且至少 2 个 BE 存活                                                            |
| 停 BE 后写入报错 `alive replica num < load required replica num` | 存活副本数低于建表时的 `replication_allocation`，Doris 默认不允许降级写入 | 属预期行为（见 [4.3 节](#43-验证单-be-存活时的写入行为会失败这是预期行为)），恢复对应 BE 或调低副本数即可继续写入 |
| 建物化视图报 `Duplicate column name`                             | SELECT 列表中的列（含 GROUP BY key）与原表列同名                          | 给列起别名，如 `status AS status_`                                                                                |
| 物化视图创建后直接 `SELECT mv_xxx` 报错                          | 同步物化视图不支持直接查询                                                | 查询原表，让优化器自动改写命中物化视图，用 `EXPLAIN` 验证是否命中                                                 |
| `docker compose stop` 后端口仍被占用                             | 容器未完全退出                                                            | `docker compose ps` 确认状态，必要时 `docker compose down` 再 `up -d`                                             |
