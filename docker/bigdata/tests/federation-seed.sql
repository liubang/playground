-- 联邦查询示例数据：Hive 维度表 + Paimon 明细表
-- 由 bigdata/bootstrap.sh 通过 spark-sql 执行（幂等，可重复运行），
-- users.user_id / events.user_id 与 Doris 内表 demo.orders.user_id 对应。

-- ========== Hive 维度表（对接 Doris hive_fed catalog） ==========
CREATE DATABASE IF NOT EXISTS demo;

CREATE TABLE IF NOT EXISTS demo.users
(
    user_id   BIGINT,
    user_name STRING,
    city      STRING,
    vip_level STRING
);

INSERT OVERWRITE TABLE demo.users VALUES
    (1001, 'Alice',   'Beijing',  'GOLD'),
    (1002, 'Bob',     'Shanghai', 'SILVER'),
    (1003, 'Charlie', 'Hangzhou', 'NORMAL'),
    (1004, 'Diana',   'Shenzhen', 'GOLD'),
    (1005, 'Eve',     'Chengdu',  'SILVER');

-- ========== Paimon 明细表（对接 Doris paimon_fed catalog） ==========
CREATE DATABASE IF NOT EXISTS paimon.demo;

CREATE TABLE IF NOT EXISTS paimon.demo.events
(
    event_id   BIGINT,
    user_id    BIGINT,
    event_type STRING,
    event_time TIMESTAMP
);

INSERT OVERWRITE TABLE paimon.demo.events VALUES
    (1, 1001, 'CLICK',   TIMESTAMP '2026-07-01 09:58:00'),
    (2, 1001, 'PAY',     TIMESTAMP '2026-07-01 10:00:05'),
    (3, 1002, 'CLICK',   TIMESTAMP '2026-07-02 11:28:00'),
    (4, 1002, 'PAY',     TIMESTAMP '2026-07-02 11:30:10'),
    (5, 1003, 'CLICK',   TIMESTAMP '2026-07-03 09:10:00'),
    (6, 1004, 'REFUND',  TIMESTAMP '2026-07-04 22:00:00');
