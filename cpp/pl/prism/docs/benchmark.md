# Prism 性能基准

对 Prism 流水线四个阶段（lex / parse / parse+print / parse+plan）的吞吐基准。
基准程序：`cpp/pl/prism/benchmark/parser_benchmark.cpp`（nanobench 4.3.11）。

## 测试数据说明

所有 workload 均为**程序化生成的全合成 SQL**：固定种子（20260914）确定性生成，
模拟线上报表/看板类长 SQL 的**结构形态**（宽 select 列、多路 JOIN、聚合/窗口
函数、UNION ALL 堆叠、嵌套子查询、深表达式），不包含任何真实业务信息——表名
一律为 `fact_f{0..3}` / `dim_d{0..7}`，列名为 `k0..k15` / `m0..m15` / `s0..s7` / `ts0`。

| workload | bytes | 形态 |
|---|---:|---|
| dashboard_s | 1,232 | 12 列宽表 + 2 JOIN + 内联聚合派生表 + GROUP BY/HAVING/窗口 |
| dashboard_m | 4,174 | 60 列 + 4 JOIN，同上看板形态 |
| dashboard_l | 14,326 | 260 列 + 6 JOIN，接近大型看板自动生成的查询 |
| union_report_m | 18,395 | 40 个聚合块 UNION ALL 堆叠 |
| union_report_l | 85,834 | 160 个聚合块 UNION ALL 堆叠 |
| expr_deep | 208,132 | 24 列深度嵌套表达式（CASE/算术/函数嵌套，深度 11） |
| subquery_nest | 1,932 | 10 层 IN/EXISTS 嵌套子查询 |

## 环境

- CPU：Apple M4 Pro（arm64）
- OS：macOS 26.6.2
- 编译器：Homebrew clang 23.1.1
- 构建：`bazel build //cpp/pl/prism/benchmark:parser_benchmark --config=release`
  （`-c opt`，关闭 ASan）
- 测量：nanobench，`unit("byte").batch(sql 字节数)`，每个条目自动收敛；
  两次全量运行结果一致（偏差 < ~5%）

## 结果（2026-09-14 采集）

| workload | lex (ns/B) | parse (ns/B) | parse+print (ns/B) | parse+plan (ns/B) |
|---|---:|---:|---:|---:|
| dashboard_s | 5.69 | 12.67 | 14.50 | 13.32 |
| dashboard_m | 6.60 | 13.69 | 15.37 | 14.10 |
| dashboard_l | 7.00 | 14.15 | 15.90 | 14.56 |
| union_report_m | 6.84 | 13.66 | 15.36 | 14.20 |
| union_report_l | 7.01 | 13.98 | 15.94 | 14.41 |
| expr_deep | 6.23 | 13.65 | 15.38 | 14.04 |
| subquery_nest | 5.47 | 13.20 | 14.56 | 13.38 |

换算为吞吐与单条耗时（parse 阶段）：

| workload | parse 吞吐 | 单条 parse 耗时 |
|---|---:|---:|
| dashboard_s (1.2 KB) | ~79 MB/s | ~15.6 µs |
| dashboard_m (4.2 KB) | ~73 MB/s | ~57 µs |
| dashboard_l (14 KB) | ~71 MB/s | ~203 µs |
| union_report_l (86 KB) | ~72 MB/s | ~1.2 ms |
| expr_deep (208 KB) | ~73 MB/s | ~2.8 ms |

## 观察

- **parse 吞吐与查询形态基本无关**：1 KB 到 208 KB、从堆叠 UNION ALL 到深
  嵌套表达式，全部落在 13.2–14.2 ns/B（70–79 MB/s）的窄带内，说明递归下降 +
  arena 分配的每字节成本稳定，无常数项以外的路径恶化。
- **lex 约为 parse 的两倍速**（5.5–7.0 ns/B，140–183 MB/s），符合预期：词法
  层是单次扫描 + 关键字二分查表，parse 的主要成本在 AST 构建与递归调用。
- **print 增量约 +1.3 ns/B**（parse+print ≈ 15.4 ns/B）：优先级最小括号化
  的访问者输出接近零开销，吞吐 ~63–69 MB/s。
- **plan 增量约 +0.4 ns/B**：纯结构变换几乎免费，聚合/窗口表达式未做符号
  提取，仅浅拷贝引用。
- 小查询（1 KB 级）单条 ~16 µs，略高于 design.md 中“典型查询 < 10µs”的
  目标——该量级下固定开销（tokens vector、首个 arena block 分配）占比显著；
  4 KB 以上进入稳定的每字节成本。吞吐目标（10–100 MB/s）达成。

## 复现

```bash
bazel build //cpp/pl/prism/benchmark:parser_benchmark --config=release
bazel-bin/cpp/pl/prism/benchmark/parser_benchmark
```

注意：不要用默认配置跑（默认开启 ASan 且为 debug 编译，数字会差 5–10 倍）。
