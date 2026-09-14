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

## 与 Trino parser 的对比（2026-09-14 采集）

对照组：`io.trino:trino-parser:468`（ANTLR4 生成解析器），跑**完全相同的
7 个 workload**（`--dump` 导出的 `cpp/pl/prism/benchmark/workloads/*.sql`，
Java 侧经 runfiles 读取）。Trino 不暴露独立的 lexer 阶段，`trino-parser`
artifact 也没有逻辑计划层，因此只对比 parse 与 parse+print 两段。

- 基准程序：`java/pl/prism/TrinoParserBenchmark.java`
- 运行：`bazel run //java/pl/prism:trino_parser_benchmark --java_runtime_version=remotejdk_25`
  （trino-parser:468 的 class file 需要 JDK 23+，默认运行时 toolchain 是 JDK 21）
- JDK：OpenJDK 25（Temurin，rules_java remotejdk25），同一台 Apple M4 Pro
- 方法：JIT 预热 1s，5 个 epoch（每个 ~200ms）取中位数

| workload | Prism parse | Trino parse | 倍数 | Prism parse+print | Trino parse+format | 倍数 |
|---|---:|---:|---:|---:|---:|---:|
| dashboard_s | 12.67 ns/B | 120.37 ns/B | **9.5x** | 14.50 ns/B | 216.57 ns/B | **14.9x** |
| dashboard_m | 13.69 ns/B | 118.54 ns/B | 8.7x | 15.37 ns/B | 212.07 ns/B | 13.8x |
| dashboard_l | 14.15 ns/B | 117.34 ns/B | 8.3x | 15.90 ns/B | 215.67 ns/B | 13.6x |
| union_report_m | 13.66 ns/B | 117.15 ns/B | 8.6x | 15.36 ns/B | 222.41 ns/B | 14.5x |
| union_report_l | 13.98 ns/B | 119.56 ns/B | 8.6x | 15.94 ns/B | 226.36 ns/B | 14.2x |
| expr_deep | 13.65 ns/B | 340.17 ns/B | **24.9x** | 15.38 ns/B | 392.83 ns/B | **25.5x** |
| subquery_nest | 13.20 ns/B | 125.11 ns/B | 9.5x | 14.56 ns/B | 206.19 ns/B | 14.2x |

换算吞吐：Trino parse ~8.0–8.5 MB/s（expr_deep 2.9 MB/s），Prism ~70–79 MB/s。

### 对比观察

- **常量差约 9 倍**：常规形态下 Trino 稳定在 ~117–125 ns/B。差距来源主要
  是解析器生成方式（ANTLR ALL(*) 预测 vs 手写递归下降 + 关键字二分）和内存
  模型（每节点堆分配 + GC vs arena 块分配）。
- **深表达式放大到 25 倍**：expr_deep（深度 11 的嵌套表达式）上 Trino 恶化
  到 340 ns/B，Prism 仍是 13.65 ns/B——ANTLR 的自适应预测在深嵌套文法上
  代价显著，而 Prism 的 precedence climbing 每字节成本与嵌套深度无关。
- Trino 的 formatter 增量（~95 ns/B）远大于 Prism printer（~1.3 ns/B），
  因为 SqlFormatter 做完整的美化布局，而 Prism printer 只做优先级最小
  括号化的线性输出——两者定位不同，此项仅供参考。
- 公平性说明：JVM 侧已充分预热（数字为 JIT 稳态），GC 默认参数；
  Trino 侧字节数含 dump 文件末尾换行（+1B，可忽略）。
