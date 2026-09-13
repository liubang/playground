# Prism 设计文档

Prism 是一个基于 C++20 的高性能 Trino SQL 解析器：一束 SQL 进来，色散成多种方言输出。

## 目标

1. **完整解析 Trino SQL**：以 Trino 官方 ANTLR 文法 SqlBase.g4 为兼容性基准，初始基线锁定 Trino 476，覆盖查询、DML、DDL、DCL、会话管理全部语句类型。
2. **生成语法树（AST）**：无损保留源位置信息（offset/line/column），支持 round-trip（AST → SQL 文本与原输入语义等价）。
3. **生成结构化逻辑计划**：纯结构变换，不依赖 catalog。计划节点包括：

```
Project / Filter / Join / Aggregation / Window / Union / Sort / Limit / TableScan / Values
```

4. **方言改写**：在 AST 上做多方言 unparse，首个目标方言 Spark SQL，架构上支持增量扩展（Hive、Doris、StarRocks 等）。
5. **高性能**：手写递归下降 + arena 分配，典型查询（几 KB）解析微秒级，AST 零 shared_ptr、零虚函数派发热点。

## 非目标

- 不做完整语义分析（名字绑定、类型推导、coercion 规则）。这是 Trino StatementAnalyzer 的范畴，依赖 catalog 元数据，作为远期可选项（P4）。
- 不做查询执行、不做物理计划与优化器。
- 不兼容 Trino 的 TypeSignature 全套规则。

## 总体架构

```
            ┌─────────────────────────────────────────────────┐
  SQL text  │  syntax/              plan/          dialect/    │  SQL text
 ──────────►│  lexer → parser ──┐   logical plan   unparser    │ ──────────►
            │        ↓          │      ↑                        │  (Trino /
            │       AST ────────┼──────┘           AstVisitor   │   Spark / …)
            └─────────────────────────────────────────────────┘
```

- **syntax/**：词法 + 语法 + AST，唯一理解 Trino 文法的层。
- **plan/**：AST → 结构化逻辑计划（纯结构变换，无 catalog 依赖）。
- **dialect/**：AST → 目标方言 SQL 文本。visitor + per-dialect 配置（函数映射表 + 特性开关）。

层间单向依赖：syntax 不依赖任何上层；plan、dialect 只依赖 syntax。

## 目录结构

```
cpp/pl/prism/
├── docs/            # 设计文档
├── syntax/          # token / lexer / ast / parser
├── plan/            # 逻辑计划节点 + AstToPlan 变换        (P3)
├── dialect/         # unparser 框架 + trino/spark 方言    (P3)
├── cli/             # prism 命令行：parse / explain / rewrite
└── ut/              # 单元测试，镜像源码目录结构
```

## 词法层（syntax/lexer）

**手写 table-driven scanner，不使用 Ragel。** 理由：

- Trino 词法完全上下文无关（无 InfluxQL 那种正则字面量 vs 除号的歧义），Ragel 的多状态机能力用不上。
- Trino 有 250+ 关键字且大小写不敏感。Ragel 把关键字编进 DFA 会导致状态数爆炸；标准做法是**扫描出 identifier 后做编译期哈希查表**。
- 手写 scanner 能给出精确的词法错误诊断（未闭合字符串、非法 unicode 转义等），Ragel 只有笼统的 error 状态。
- 少一层 .rl → .cpp 的 codegen，构建与 clangd 体验更顺。

### Token 设计

零拷贝：token 只存指向原文的区间 [offset, length)，不持有字符串。

```cpp
struct Token {
    TokenType type;
    uint32_t offset;   // 在源文本中的字节偏移
    uint32_t length;   // 字节长度
    uint32_t line;     // 1-based
    uint32_t column;   // 1-based
    std::string_view text(std::string_view source) const;
};
```

### 关键字策略

- 词法层只识别 **reserved keywords**：编译期排序表 + 二分查找，identifier 长度大于 17 直接跳过查表（Trino 最长保留字 CURRENT_TIMESTAMP 为 17 字符）。
- **non-reserved keywords**（FIRST、LAST、VARCHAR、DATE 等）词法层一律输出 identifier token，由 parser 在语法位置做"软关键字"判定。这与 Trino 文法 nonReserved 规则语义一致，避免词法层过度承诺。

### 词法要素

| 要素   | 规则                                                                         |
| ------ | ---------------------------------------------------------------------------- |
| 标识符 | [a-zA-Z_][a-zA-Z0-9_$]*；delimited identifier 用双引号包裹，双写双引号转义   |
| 字符串 | 单引号包裹，双写单引号转义；U&'...' 为 unicode 字符串；X'...' 为二进制字面量 |
| 数字   | DIGIT+ ('.' DIGIT*)? ([eE][+-]? DIGIT+)?，或 '.' DIGIT+ 带可选指数           |
| 注释   | -- 行注释；/* */ 块注释（Trino 不嵌套）                                      |
| 运算符 | 最长 3 字符：<>、!=、<=、>=、                                                |     | 、::、->、=> 等 |

## 语法层（syntax/parser）

手写递归下降 + **precedence climbing** 表达式解析，参考 Trino 文法翻译为代码结构：

- **表达式**：约 20 级优先级，从高到低大致为：

```
primary → 下标/字段访问 → AT TIME ZONE → 一元正负 → 乘除模 → 加减
       → BETWEEN / IN / LIKE / IS → NOT → AND → OR
```

- **Lambda**：

```sql
x -> x + 1
(a, b) -> a + b
```

与括号表达式存在歧义，用"speculative parse + 回溯"处理：括号列表解析后看下一个 token 是否为 ->。

- **语句分类**：首 token 分派：

```
SELECT / WITH / VALUES / TABLE       → query
CREATE / ALTER / DROP                → DDL
INSERT / UPDATE / DELETE / MERGE     → DML
GRANT / REVOKE / DENY                → DCL
SET / RESET / SHOW / USE / ...       → 会话命令
```

- **错误恢复**：statement 级 panic mode（跳到下一个分号），收集所有错误而非遇错即停。

## AST 设计（syntax/ast）

- 节点体系对齐 Trino trino-parser 的 Node 层次：

```
Statement
 └── Query
      ├── QuerySpecification   -- SELECT ... FROM ... WHERE ...
      ├── SetOperation         -- UNION / INTERSECT / EXCEPT
      └── Values
Expression                     -- 独立一棵体系
```

- **内存**：arena 分配（复用 cpp/pl/arena 的思路），整棵 AST 一次释放；节点内用裸指针，不用 shared_ptr。
- **遍历**：CRTP 静态 visitor，编译期派发，无虚函数开销：

```cpp
template <typename R, typename Derived>
class AstVisitor { /* static dispatch on node type */ };
```

- 每个节点携带源位置信息，支撑精准报错与方言改写时的位置保留：

```cpp
struct SourceLocation {
    uint32_t offset;
    uint32_t line;
    uint32_t column;
};
```

## 逻辑计划（plan/）

AST → PlanNode 纯结构变换：

```
QuerySpecification → Project(select) → Filter(where) → Aggregation(group by/having)
                   → Sort(order by) → Limit
FROM 子句          → JoinTree / TableScan / Unnest
```

不绑定 catalog、不推导类型；列引用保持 DereferenceExpression 的符号形态。

## 方言改写（dialect/）

- SqlDialect 抽象：函数名映射表、特性开关（是否支持 UNNEST、ILIKE、标识符引号风格）、字面量渲染规则。函数映射示例：

```
approx_distinct  →  approx_count_distinct   (Trino → Spark)
```

- Unparser = AST visitor + dialect 配置；Trino 方言自身也是第一个 unparser 实现（用于 round-trip 测试）。
- **已知结构性差异点**（Trino → Spark）：

| Trino                                | Spark SQL                                           |
| ------------------------------------ | --------------------------------------------------- |
| UNNEST(arr)                          | LATERAL VIEW explode(arr)（结构性重写，非简单替换） |
| approx_distinct(x)                   | approx_count_distinct(x)                            |
| VARCHAR                              | STRING                                              |
| ASC 默认 NULLS LAST                  | ASC 默认 NULLS FIRST（需显式补 NULLS LAST）         |
| regexp_extract(s, p)（默认 group 0） | regexp_extract(s, p, 0)（group 必填差异）           |

## 性能设计

- 词法：256 项字符分类表 + 单次扫描，token 零分配。
- 语法：递归下降无回溯（仅 lambda/软关键字等少数点做 speculative parse）。
- 内存：arena 单块增长分配，AST 节点连续排布，缓存友好。
- 目标：单条典型查询（几 KB）端到端 parse 小于 10µs；吞吐 10–100 MB/s SQL 文本。
- benchmark 放 syntax/benchmark/（基于 nanobench），后续补 CI 性能回归。

## 测试策略

1. **单元测试**：每层独立 cc_test（gtest），镜像源码目录放在 ut/。
2. **黄金语料**：从 Trino 官方 TestSqlParser / SqlFormatter 测试导出 SQL 语句语料，做两类断言：
   - parse 成功（兼容性）；
   - round-trip（parse → unparse → 再 parse，两棵 AST 结构等价）。
3. **方言改写测试**：(trino_sql, spark_sql) 对拍语料，人工维护核心差异点 + 语料回归。
4. **fuzz**（远期）：随机 AST 生成 → unparse → reparse 不变量校验。

## Roadmap

| 阶段 | 内容                                                        | 产出              |
| ---- | ----------------------------------------------------------- | ----------------- |
| P0   | lexer + 表达式 + SELECT 主干（JOIN/CTE/窗口/UNNEST/子查询） | 覆盖 60% 真实查询 |
| P1   | DML（INSERT/DELETE/UPDATE/MERGE）+ 常用 DDL                 |                   |
| P2   | DDL/DCL/会话命令全量长尾                                    | 语法兼容达成      |
| P3   | 逻辑计划 + Trino/Spark unparser                             | 改写能力达成      |
| P4   | （可选）语义分析：名字绑定 + 类型推导                       |                   |

## 版本兼容基线

- 初始基线：**Trino 476** 的 SqlBase.g4。
- 升级策略：跟随 Trino release 滚动；每次升级在 docs/ 记录新增语法点与对应 AST 变更。
