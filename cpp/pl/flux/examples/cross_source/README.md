# Cross-source examples

This directory contains runnable provider-package examples that combine SQL
sources, CSV dimensions, and in-memory array data.

SQLite examples run directly against the checked-in `metrics.db`. MySQL examples
expect a local MySQL database seeded with `mysql_metrics.sql`:

```bash
mysql -h127.0.0.1 -P3306 -uflux -pflux flux_test \
  < cpp/pl/flux/examples/cross_source/mysql_metrics.sql
```

The MySQL `.flux` files default to the same connection shape used by CI:
`127.0.0.1:3306`, user `flux`, password `flux`, database `flux_test`. For a
different local server, edit the five `mysqlHost` / `mysqlPort` / `mysqlUser` /
`mysqlPassword` / `mysqlDatabase` bindings at the top of each MySQL example.

Examples:

- `sqlite_pushdown_explain.flux`: SQLite source pushdown and `explain()` output.
- `sqlite_csv_join.flux`: SQLite metrics joined with CSV ownership metadata.
- `sqlite_csv_array_incidents.flux`: SQLite, CSV, and array threshold data.
- `mysql_pushdown_explain.flux`: MySQL source pushdown and `explain()` output.
- `mysql_csv_join.flux`: MySQL metrics joined with CSV ownership metadata.
- `mysql_sqlite_csv_array_incidents.flux`: MySQL live metrics, SQLite baseline
  metrics, CSV ownership metadata, and array thresholds in one incident-style
  query.
