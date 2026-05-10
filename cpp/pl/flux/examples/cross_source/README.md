# 跨源查询示例

这个目录放的是可以直接运行的 provider package 示例，覆盖 SQL 数据源、CSV 维表和
内存里的 array 数据组合。

SQLite 示例会直接读取仓库里的 `metrics.db`。MySQL 示例需要先在本地 MySQL
数据库中导入 `mysql_metrics.sql`：

```bash
mysql -h127.0.0.1 -P3306 -uflux -pflux flux_test \
  < cpp/pl/flux/examples/cross_source/mysql_metrics.sql
```

MySQL `.flux` 文件默认使用和 CI 一样的连接配置：`127.0.0.1:3306`，用户
`flux`，密码 `flux`，数据库 `flux_test`。如果要连其他本地 MySQL server，
改每个 MySQL 示例开头的 `mysqlHost` / `mysqlPort` / `mysqlUser` /
`mysqlPassword` / `mysqlDatabase` 这五个绑定即可。

示例列表：

- `sqlite_pushdown_explain.flux`：SQLite 数据源下推和 `explain()` 输出。
- `sqlite_csv_join.flux`：SQLite 指标表 join CSV owner 维表。
- `sqlite_csv_array_incidents.flux`：SQLite 指标、CSV owner 维表和 array 阈值表组合。
- `mysql_pushdown_explain.flux`：MySQL 数据源下推和 `explain()` 输出。
- `mysql_csv_join.flux`：MySQL 指标表 join CSV owner 维表。
- `mysql_sqlite_csv_array_incidents.flux`：MySQL 实时指标、SQLite 基线指标、
  CSV owner 维表和 array 阈值表组合成一个事件排查风格查询。
