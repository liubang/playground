-- 示例库表初始化脚本
-- 由 BE 容器启动流程自动执行一次（挂载于 /docker-entrypoint-initdb.d），
-- 用于快速验证集群可用性，与生产建表逻辑无关，可按需修改或删除。

CREATE DATABASE IF NOT EXISTS demo;

CREATE TABLE IF NOT EXISTS demo.orders
(
    order_id    BIGINT,
    user_id     BIGINT,
    amount      DECIMAL(10, 2),
    order_time  DATETIME,
    status      VARCHAR(20)
)
DUPLICATE KEY(order_id)
DISTRIBUTED BY HASH(order_id) BUCKETS 4
PROPERTIES (
    "replication_allocation" = "tag.location.default: 2"
);

INSERT INTO demo.orders (order_id, user_id, amount, order_time, status) VALUES
    (1, 1001, 29.90,  '2026-07-01 10:00:00', 'PAID'),
    (2, 1002, 158.00, '2026-07-02 11:30:00', 'PAID'),
    (3, 1001, 9.90,   '2026-07-03 09:15:00', 'CANCELLED'),
    (4, 1003, 88.50,  '2026-07-04 20:45:00', 'PAID'),
    (5, 1002, 12.00,  '2026-07-05 08:05:00', 'PENDING');
