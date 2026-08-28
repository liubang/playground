-- 外表数据源示例数据
-- 由 MySQL 容器首次启动时自动执行（挂载于 /docker-entrypoint-initdb.d），
-- users.user_id 与 Doris 内表 demo.orders.user_id 对应，用于验证内外表联邦 JOIN。

CREATE DATABASE IF NOT EXISTS federation_demo;

CREATE TABLE IF NOT EXISTS federation_demo.users
(
    user_id   BIGINT      NOT NULL PRIMARY KEY,
    user_name VARCHAR(64) NOT NULL,
    city      VARCHAR(64),
    vip_level VARCHAR(16)
);

INSERT INTO federation_demo.users (user_id, user_name, city, vip_level) VALUES
    (1001, 'Alice',   'Beijing',  'GOLD'),
    (1002, 'Bob',     'Shanghai', 'SILVER'),
    (1003, 'Charlie', 'Hangzhou', 'NORMAL'),
    (1004, 'Diana',   'Shenzhen', 'GOLD');

-- 1005 不在内表订单中，用于验证 LEFT JOIN 语义（对外表侧数据更全的场景）
INSERT INTO federation_demo.users (user_id, user_name, city, vip_level) VALUES
    (1005, 'Eve', 'Chengdu', 'SILVER');
