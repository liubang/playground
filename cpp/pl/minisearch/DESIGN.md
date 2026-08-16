# MiniSearch Design Document

> Hybrid Search Service: FAISS + 自建倒排索引 + BM25 + RRF + Rerank | Module: `cpp/pl/minisearch`（原 `cpp/pl/recall`）

| Field   | Value                          |
| ------- | ------------------------------ |
| Author  | liubang (it.liubang@gmail.com) |
| Version | 5.0                            |
| Status  | Draft                          |

---

## 1. Background & Evolution

| Version | Milestone | Summary |
| ------- | --------- | ------- |
| v1–v2   | 专用向量召回 | FAISS + brpc HTTP/JSON，服务 NL2SQL 场景的库表召回 |
| v3.0    | Embedding 集成层 | 服务端对接 OpenAI 兼容 `/v1/embeddings`，客户端可直传文本 |
| v4.0    | MLX 本地推理 | 自带 Apple Silicon 本地 embedding server（BGE-M3），端到端零外部依赖 |
| **v5.0** | **通用混合检索服务** | **多 Collection、文档数据模型、中文倒排（jieba + BM25）、hybrid 检索（BM25 + 向量 + RRF + Rerank）、正排过滤、自动 checkpoint、MCP 接入** |

v5.0 的核心动机：服务从单一场景的向量召回 API 演进为通用检索服务。直接驱动力是 loom 知识库场景：个人知识文档（中文为主）建成可被 agent 自主检索的知识库，中文检索效果（分词、BM25、语义、重排）为核心目标。由此带来的通用化诉求：多命名空间隔离（loom KB / 库表召回 / 代码检索各一个 collection）、文档级 CRUD、hybrid 检索管道、字段过滤。

架构定位：迷你存储引擎 + 三种索引（倒排 / 向量 / docvalues）+ 打分器。segment 化为存储引擎的演进方向，M1 前不引入（见 §5、§14）。

## 2. Design Goals & Non-Goals

### 2.1 Goals

1. **通用数据模型**：多 Collection、Schema 化字段（text/keyword/numeric/vector）、文档 upsert/delete。
2. **中文检索**：可插拔 analyzer 管道（jieba + 用户词典 + 同义词），BM25 参数可调，`queries:analyze` 调试端点。
3. **Hybrid 检索**：BM25 与向量检索并行执行，RRF 融合，可选 cross-encoder rerank，全链路可降级（embedding/rerank 不可用时自动退化，服务继续可用）。
4. **单机自包含**：单进程、本地磁盘持久化、自动 checkpoint；Apple Silicon 上可全本地运行（MLX embedding）。
5. **多形态接入**：HTTP/JSON API（v2，全新实现、无 legacy 兼容层，见 §9.3）、MCP server（供 loom / Claude Code / Cursor 复用）。
6. **效果可度量**：金标准评测集 + 回归脚本，任何 analyzer/权重/模型变更必须过评测。
7. **认证与多租户**：API key 认证（Bearer），key→tenant 隐式绑定，四角色（admin / tenant_admin / writer / reader）+ collection 白名单，最小配额集（见 §10）；`--auth=off` 时单机模式行为不变。

### 2.2 Non-Goals（明确不做）

- **分布式**：不做分片、副本、Raft。单机 + checkpoint 满足目标场景。
- **查询 DSL**：保持参数化 JSON 请求（`text + filter + top_k + weights`），不自造查询语言。
- **企业级权限体系**：不做细粒度 RBAC、SSO/OIDC、组织架构同步、配额计费、TLS 终结（私有部署或反向代理场景自行处理）。四种静态角色 + collection 白名单覆盖目标场景（§10）。
- **实时流摄入 / 高写入吞吐**：目标写入频率是每天数十到数千次 upsert，不为它做复杂优化。
- **地理/时间序列/聚合**：不是通用数据库，只做检索。

## 3. Architecture Overview

```
                        ┌──────────────────────────────────────────────────────┐
                        │                  MiniSearch Server                   │
                        │                                                      │
  HTTP/JSON (v2)        │  ┌────────────────────────────────────────────────┐  │
  Bearer msk_...        │  │            API Layer (brpc, json2pb)           │  │
  MCP (stdio)  ────────▶│  │  auth middleware: key → tenant/role (§10)      │  │
                        │  │  collections / documents / search / analyze    │  │
                        │  └───────────────┬────────────────────────────────┘  │
                        │                  │                                   │
                        │  ┌───────────────▼────────────────────────────────┐  │
                        │  │              Query Pipeline                    │  │
                        │  │  analyzer ─┬─ Inverted Index (BM25) ─┐         │  │
                        │  │            └─ Vector Index (FAISS) ──┼─ RRF ─▶ │──┼──▶ hits
                        │  │  filter pushdown (docvalues/IDSelector)│       │  │
                        │  │            optional rerank + highlight ▼        │  │
                        │  └───────────────┬────────────────────────────────┘  │
                        │                  │                                   │
                        │  ┌───────────────▼────────────────────────────────┐  │
                        │  │           Storage Engine                       │  │
                        │  │  Collection Registry                           │  │
                        │  │  MemTable ─▶ Immutable Segments ─▶ (merge)     │  │
                        │  │  docstore | inverted | vector | docvalues      │  │
                        │  │  Manifest + Checkpoint + WAL (M3)              │  │
                        │  └───────────────┬────────────────────────────────┘  │
                        │                  │                                   │
                        │  ┌───────────────▼──────┐   ┌────────────────────┐  │
                        │  │  Embedding (per-     │   │  Rerank (optional) │  │
                        │  │  collection, cached) │   │  cross-encoder API │  │
                        │  └───────────────┬──────┘   └────────────────────┘  │
                        └──────────────────┼───────────────────────────────────┘
                                           │ OpenAI 兼容 /v1/embeddings
                                           ▼
                          ┌─────────────────────────────────┐
                          │  MLX Embedding Server（本项目自带 │
                          │  BGE-M3 / bge-large-zh，本地推理）│
                          │  或 Ollama / OpenAI 兼容 API       │
                          └─────────────────────────────────┘
```

组件职责：

| Component            | Responsibility                                                       |
| -------------------- | -------------------------------------------------------------------- |
| **API Layer**        | brpc HTTP/JSON（复用 v4 的 `default_method` + json2pb 模式），路由分发 |
| **Auth & Tenancy**   | API key 认证中间件（key→tenant/role）、租户与 Key 管理、配额（§10）  |
| **Query Pipeline**   | 查询解析、analyzer、BM25/ANN 并行检索、filter 下推、RRF、rerank、高亮 |
| **Collection Registry** | collection 元数据（schema/settings）管理，进程内单例               |
| **Storage Engine**   | 文档与索引的存储、持久化、compaction（M1 简化形态，M3 segment 化）   |
| **EmbeddingClient**  | 复用 v4 抽象，per-collection 配置，query 结果 LRU 缓存                |
| **RerankClient**     | OpenAI/Cohere 兼容 rerank API 客户端（远端 API 或本地模型），可选            |
| **MCP Endpoint**     | 独立 binary，stdio MCP server，包装 search/list/get 工具             |

## 4. Data Model

### 4.1 Collection

```json
{
  "name": "loom-kb",
  "schema": {
    "fields": {
      "title":    {"type": "text", "indexed": true, "stored": true},
      "content":  {"type": "text", "indexed": true, "stored": true, "analyzer": "cjk_jieba"},
      "source":   {"type": "keyword", "indexed": true, "stored": true},
      "tags":     {"type": "keyword", "indexed": true, "stored": false},
      "created":  {"type": "numeric", "indexed": false, "stored": true},
      "content_vec": {"type": "vector", "dims": 1024, "metric": "cosine",
                       "source": {"field": "content", "mode": "server"}}
    }
  },
  "settings": {
    "analyzer": "cjk_jieba",
    "embedding": {"endpoint": "http://localhost:8000", "model": "bge-m3",
                  "query_prefix": "", "doc_prefix": ""},
    "bm25": {"k1": 1.2, "b": 0.75},
    "vector_weight": 1.0, "bm25_weight": 1.0, "rrf_k": 60
  }
}
```

- **text**：分词后进倒排；`analyzer` 可按字段覆盖（如代码片段字段用空白分词）。
- **keyword**：不分词，精确匹配，用于 filter 与未来聚合。
- **numeric**：范围 filter。
- **vector**：两种来源——`mode: "server"`（服务端对 `source.field` 自动 embedding，复用 v4 文本直通模式）或 `mode: "client"`（客户端在文档 `fields.<vec_field>` 中直接传 `float` 数组，对应 v4 向量模式，此时 schema 无需 `source`）。**向量维度与 metric 在 schema 创建时锁定**。
- `query_prefix` / `doc_prefix`：BGE 系列模型对短查询可能需要 instruction 前缀（如 bge-large-zh 的检索指令），per-collection 配置，评测集上验证后设定。

### 4.2 Document

```json
{
  "id": "docs/presto-tuning.md#L23",     // 客户端定义，collection 内唯一
  "version": 1723737600000,              // 可选；后写覆盖先写（LWW），用于幂等 upsert
  "fields": {"title": "Presto 调优", "content": "当 CPU 热点出现在 join 阶段……", "source": "wiki"},
  "payload": {...}                        // 不索引、原样存储、检索时返回
}
```

- **upsert 语义**：同 id 写入即替换（M1：tombstone 旧版本 + 追加新版本，查询时去重取最新）。
- **delete**：tombstone（M1：内存 hash set + 查询过滤；M3：进 segment 并在 merge 时压实）。
- v4 兼容映射：`table_id` → `id`，`meta` → `payload`，向量 → 单一 vector 字段。

### 4.3 DocID 内部表示

沿用 v4 `IdMapper` 思路但**语义有一处关键演进**：collection 内字符串 id ↔ int64 内部 docid，映射表随 collection 持久化；倒排、docvalues、向量块全部使用内部 docid，杜绝存储层出现变长字符串键。

- **upsert 重分配语义**：同一字符串 id 再次 upsert 时分配**新的**内部 docid，旧 docid 进 tombstone（§4.2）。v4 `IdMapper::get_or_assign` 的幂等语义（同 id 恒返回同值）在此不再适用，需增加 `reassign`；FAISS 的 `IndexIDMap` 对同 id 重复 add 会产生重复向量，因此 upsert 必须走"新 docid + 旧 docid tombstone"路径，物理清除推迟到 checkpoint 重建（§15-5）。
- docid 作用域：M1 为 collection 级单调递增；M3 演进为 segment 内局部 docid + 全局映射（见 §5）。

## 5. Storage Engine

### 5.1 两阶段演进策略

**M1 简化形态**：

```
Collection (in-memory)
  ├── 活跃内存索引：增量倒排（hash map: term → posting vector）
  ├── FAISS IndexIDMap（增量 add）
  ├── docstore：unordered_map<int64, Document>（payload + stored fields）
  ├── tombstones：hash set<int64>（delete/旧版本）
  └── 周期 checkpoint（复用 v4 snapshot 机制，自动触发：N 次写或 T 秒）
```

- 查询 = 内存倒排扫描 + FAISS 检索；结果收敛规则：内部 docid → 字符串 id，按字符串 id 滤 tombstone 旧版本、保留最新 version（§7.1 第 5 步）。
- 写路径全程 collection 级 `std::shared_mutex` 写锁，读路径读锁。个人场景读写比极高，M1 不做更细粒度并发。
- **checkpoint 自动化**：定时器每 T=60s 检查一次 dirty 标志（有写入未 checkpoint），距上次成功 checkpoint 写入量 ≥ N=1000 或时间 ≥ 300s 即触发；checkpoint 序列化期间持写锁（个人场景写入稀疏，阻塞可忽略），完成后原子更新 manifest 并滚动保留最近 2 个 checkpoint（旧的删除）。已删/被替换文档在 dump 时自然排除（tombstone 不落盘），等效于 checkpoint 时物理清除。对比 v4 的手动 save/load，重启零丢失。

**M3 完整形态（条件触发，见 §14）**：MemTable → freeze → Immutable Segment（列式布局）→ 后台 merge（LSM 思维）。触发条件：单 collection 文档数 > 50 万，或写入频率使 checkpoint 全量序列化超过秒级。届时：

- **倒排段内布局**：term 字典（FST 或排序数组 + 前缀压缩）+ posting list（docid delta + PFor/SIMD-BP128 压缩，复用 sstv2 block codec 经验）。
- **向量段内布局**：连续 float32/f16 列存，段级 FAISS 子索引，merge 时重建。
- **docvalues**：filter 字段列存（bitmap 或 sort-based），支持谓词下推。
- **Manifest**：segment 文件集合 + 创建/删除位 点，checkpoint 原子切换（写临时 manifest + rename）。
- **WAL**：仅 M3 引入（M1 靠高频 checkpoint 已足够，单机个人场景可容忍分钟级重放窗口）。

### 5.2 持久化文件布局

```
<data_dir>/minisearch/
  ├── auth/
  │   └── keys.json                    # key store（sha256 + 元数据，原子写，§10.3）
  └── <tenant>/
      └── <collection>/
          ├── manifest.json            # schema + settings + 文件清单 + checkpoint 元信息
          ├── checkpoint.<seq>.faiss   # 向量索引（faiss::write_index 格式，复用 v4）
          ├── checkpoint.<seq>.inv     # 倒排序列化（M1：二进制 dump）
          ├── checkpoint.<seq>.docs    # docstore 序列化（protobuf length-prefixed）
          └── idmap.<seq>.bin          # v4 IdMapper 格式复用
```

## 6. Text Analysis Pipeline

管道可插拔，query 与 doc 可使用不同 analyzer。

### 6.1 Analyzer 接口

```cpp
// analysis/analyzer.h
class Analyzer {
public:
    virtual ~Analyzer() = default;
    // 输入原始文本，输出 token 序列（保留位置偏移，供高亮使用）
    virtual std::vector<Token> Analyze(std::string_view text) const = 0;
    struct Token { std::string term; uint32_t pos; uint32_t begin; uint32_t end; };
};
// 注册制：AnalyzerRegistry::Create("cjk_jieba", config)
```

内置实现（按里程碑）：

| Analyzer          | 机制                                                     | Milestone |
| ----------------- | -------------------------------------------------------- | --------- |
| `raw`             | 空白切分（代码/标识符场景）                              | M0        |
| `cjk_jieba`       | Jieba-CPP（词典 MP + HMM 新词识别；zstd 嵌入词典）        | M1（bazel 集成已完成，registry `jieba_cpp` 0.1.0）|
| `cjk_jieba_syn`   | jieba + 同义词表扩展（多生成同义 term，位置不变）        | M2        |
| `bigram_fallback` | 2 字短查询兜底（分词 miss 时 bigram 扩展，见 §7.4）        | M1        |

### 6.2 词典与同义词运营

- **用户词典**：Jieba-CPP 的词典为预编译 darts 二进制，无运行时用户词典接口。领域术语、黑话（如 "打满/抖动/泄漏" 这类搭配）的扩展路径：(a) query 侧同义词表扩展（见下条，M1 采用）；(b) 离线重生成词典——上游 `tools/generate_dict.py` 支持导入自定义词表后重建词典二进制（M2 评估为词典运营工具）。
- **同义词表**`synonyms.txt`（`线程池,thread pool,ThreadPool` 每行一组，双向）。**只扩展 query 侧，不进索引侧**（doc 侧扩展会放大索引且破坏 BM25 词频统计）。
- 两者文件 mtime 监控热加载，改动即生效，无需重启。
- 调试端点 `POST /api/v2/{col}/queries:analyze`（§9）输入任意文本，返回分词与同义词展开结果，用于词典与同义词的调试验证。

### 6.3 归一化

- 全角→半角、大写→小写（ASCII）；中文标点切分。
- 英文驼峰拆分（`ThreadPool` → `thread pool`，仅 text 字段可配开启）——缓解中英混排术语召回。

## 7. Query Pipeline

### 7.1 执行计划

```
SearchRequest
  │
  ├─ 1. Query Analyzer（query 侧 analyzer + 同义词扩展 + 短查询兜底）
  │
  ├─ 2. 并行两路：
  │     ├─ BM25：倒排检索 top-K1（K1 = max(top_k * 5, 50)，WAND 优化后置）
  │     └─ ANN：query embedding（LRU 缓存，miss 时调 embedding API）
  │              → FAISS top-K2（K2 = max(top_k * 5, 50))
  │     （filter 应用分阶段：M1 在两路结果上后置收敛——结果集 ≤ 数百，
  │       代价可忽略；M2 建 docvalues 后下推预过滤——倒排侧交集 docid、
  │       FAISS 侧构造 IDSelector bitmap，避免"检索后过滤导致结果不足"）
  │
  ├─ 3. RRF 融合：score(d) = Σ_i w_i / (rrf_k + rank_i(d))，默认 rrf_k=60
  │     （只出现在单一列表的文档参与排序；两路都命中的自然上位）
  │
  ├─ 4. 可选 rerank：取融合后 top-N（N=50）交 cross-encoder 重打分，
  │     返回重排后的 top_k（embedding/rerank 不可用时跳过，结果标记 degraded）
  │
  ├─ 5. 版本收敛：内部 docid → 字符串 id，按字符串 id 滤 tombstone 旧版本
  │     （保留最新 version 的 docid），M1 后置 filter 谓词亦在此应用
  │
  └─ 6. 装配 hits：stored fields + payload + score + highlight（term 偏移标记）
```

### 7.2 BM25

`score(q,d) = Σ_{t∈q} IDF(t) · (tf · (k1+1)) / (tf + k1·(1-b+b·|d|/avgdl))`，`IDF(t)=ln(1+(N-df+0.5)/(df+0.5))`。默认 k1=1.2、b=0.75，per-collection 可调；中文语料上 b 的最优值常低于英文（文档长度分布不同），留给评测集调优。doc 长度 = 分词后 token 数。

### 7.3 Filter 语法

```json
{"filter": {"and": [
    {"field": "source", "op": "=", "value": "wiki"},
    {"field": "tags", "op": "in", "value": ["presto", "oncall"]},
    {"field": "created", "op": ">", "value": 1700000000}
]}}
```

M1：filter 在两路检索结果上后置应用（结果集 ≤ 数百，代价可忽略）。M2：keyword/numeric 建 docvalues 后预下推。**M1 不做 M2 的优化**——先验证 filter 的真实使用频率。

### 7.4 中文短查询兜底

已知约束（loom 调研实测）：n-gram 方案对 2 字词（"节点/内存/调优"）失效——trigram 无法覆盖 2 字符查询。MiniSearch 采用分词器而非 n-gram，"节点"作为完整 token 入索引，不受此约束。风险仅存在于**词典未收录且 HMM 未识别**的 2 字组合，兜底策略：分词结果为空或全部无 posting 命中时，对原文做 bigram 扩展查询。

### 7.5 降级矩阵

| 依赖故障          | 行为                                     | 响应标记          |
| ----------------- | ---------------------------------------- | ----------------- |
| embedding 不可用  | 跳过 ANN 路，BM25-only 返回              | `"degraded": ["vector"]` |
| rerank 不可用     | 跳过重排，RRF 序直接返回                 | `"degraded": ["rerank"]` |
| 部分文档无向量    | 该文档只参与 BM25 路（向量字段可选语义） | —                 |

BM25-only 降级路径的可用性依据：ArXiv 2602.23368（agentic 关键词检索可达 RAG 90% 以上效果，无需向量库）。

## 8. Embedding & Rerank Integration

### 8.1 Per-Collection Embedding

v4 的全局 `--embedding_endpoint` 演进为 collection settings 内配置（不同 collection 可用不同模型/维度/端点：KB 用本地 MLX BGE-M3，代码检索用专用 code-embedding 服务）。`EmbeddingClient` 抽象与 `OpenAIEmbeddingClient` 实现原样复用，按 endpoint+model 实例化并池化。

### 8.2 Query 向量缓存

LRU（容量 1024，key = hash(collection + model + query text)）。KB 场景 query 重复率高（agent 反复检索同一主题），命中率可观。doc 侧不缓存（写入一次）。

### 8.3 Rerank 客户端

`RerankClient` 抽象（`Rerank(query, docs []string) []float32`），首版实现对接 OpenAI 兼容 `/v1/rerank`（OpenAI 兼容网关或本地 rerank 服务均可）；MLX reranker（bge-reranker-v2-m3 的 MLX 移植）列为 M2 评估项。rerank 输入 = top-50 的 `title + content` 截断（512 token），输出重排序。rerank 对排序质量的贡献通常大于分词与参数调优（业界经验，以 §11 评测验证）。

### 8.4 MLX Embedding Server

v4 资产原样保留（目录随模块改名迁移），继续作为默认本地后端。其 Future 项中的 dynamic batching、模型热切换维持原计划，与本设计无耦合。

## 9. HTTP API v2

### 9.1 Endpoints

| Method | Path                              | Description                                    |
| ------ | --------------------------------- | ---------------------------------------------- |
| POST   | `/api/v2/collections`             | 创建 collection（body = §4.1 定义，tenant_admin+）|
| GET    | `/api/v2/collections`             | 列出本租户 collection 及统计                   |
| DELETE | `/api/v2/collections/{name}`      | 删除（含数据目录，tenant_admin+，`?confirm=<name>` 防误删）|
| PUT    | `/api/v2/{col}/documents/{id}`    | upsert 单文档（writer+）                       |
| POST   | `/api/v2/{col}/documents:bulk`    | 批量 upsert（batch embedding 一次调用，writer+）|
| GET    | `/api/v2/{col}/documents/{id}`    | 读取原文（reader+）                            |
| DELETE | `/api/v2/{col}/documents/{id}`    | tombstone 删除（writer+）                      |
| POST   | `/api/v2/{col}/search`            | hybrid 检索（§7.1 完整管道，reader+）           |
| POST   | `/api/v2/{col}/queries:analyze`   | analyzer 调试：返回分词 + 同义词展开（reader+）  |
| POST   | `/api/v2/admin/tenants`           | 创建租户（admin）                              |
| GET    | `/api/v2/admin/tenants`           | 列出租户与用量（admin）                        |
| DELETE | `/api/v2/admin/tenants/{name}`    | 删除租户（admin，`?confirm=<name>`）           |
| POST   | `/api/v2/admin/tenants/{t}/keys`  | 签发 API key（admin / tenant_admin，§10.5）    |
| GET    | `/api/v2/admin/tenants/{t}/keys`  | 列出 key 元数据（不返回明文）                  |
| DELETE | `/api/v2/admin/tenants/{t}/keys/{key_id}` | 吊销 key                              |
| GET    | `/api/v2/admin/stats`             | 全局/分租户统计（admin）                       |
| GET    | `/healthz`                        | 健康检查（**唯一免认证端点**）                  |

认证规则（详见 §10）：除 `/healthz` 外所有端点在 `--auth=on` 时必须携带 `Authorization: Bearer msk_...`；未认证 401、角色/白名单越权 403。`<col>` 均指**当前 key 所属租户内**的 collection，跨租户访问在鉴权层拒绝，路径不携带租户参数。

### 9.2 Search 请求/响应示例

```bash
curl -X POST http://localhost:8200/api/v2/loom-kb/search \
  -H 'Content-Type: application/json' \
  -d '{
    "text": "presto join 阶段 CPU 热点怎么排查",
    "filter": {"field": "tags", "op": "in", "value": ["presto", "oncall"]},
    "top_k": 5,
    "weights": {"bm25": 1.0, "vector": 1.0},
    "rerank": true,
    "highlight": true
  }'

# {
#   "hits": [
#     {"id": "docs/presto-tuning.md#L23", "score": 0.031,
#      "fields": {"title": "Presto 调优", "source": "wiki"},
#      "payload": {...},
#      "highlight": "当 CPU 热点出现在 <em>join</em> 阶段时，优先检查统计信息……"},
#     ...
#   ],
#   "took_ms": 42, "degraded": []
# }
```

请求级参数覆盖 collection settings 默认值：`weights`/`rrf_k` 未传时取 collection settings（§4.1），`weights` 传 `{"bm25": 0}` 或 `{"vector": 0}` 即显式关闭某一路（等价于单路检索，可用于评测基线）。

### 9.3 无 Legacy 兼容层

不保留 v4 `/api/recall/*` 接口，服务按 §9.1 的 v2 API 全新实现。v4 资产处置见 Appendix A。M0 不实现适配层；`test.sh` 的 19 个 v4 用例重写为 v2 语义的功能回归。

### 9.4 MCP Endpoint

独立 binary `minisearch_mcp`（stdio transport），暴露三个工具：

- `search(collection, query, top_k)` → hits 摘要（id + title + score + 截断正文）
- `get_document(collection, id)` → 原文
- `list_collections()` → 名称 + 描述 + 文档数

loom 侧配置 MCP server 即接入（loom 的 MCP client 已支持 stdio/HTTP，见 loom `internal/mcp`）；Claude Code / Cursor 同样可直接挂载。工具描述文案引导模型先 search 定位、再 get_document 读全文，控制上下文用量。

MCP server 是 HTTP API 的薄壳，认证随配置透传：启动参数 `--endpoint` + `--api-key`（或环境变量 `MINISEARCH_API_KEY`），`--auth=on` 时必填；租户即该 key 所属租户，工具面不暴露任何跨租户操作。

## 10. Authentication & Multi-Tenancy

### 10.1 模型：Key → Tenant 隐式绑定

- **API key 是唯一凭据**：`Authorization: Bearer msk_<43 chars>`。key 签发时绑定 tenant 与 role，请求上下文（Principal）由此推导。
- **租户不出现在 URL 路径**：`/api/v2/{col}/...` 中的 collection 永远解析为当前 key 所属租户内的 collection。效果：URL 不含租户名；跨租户请求在鉴权层拒绝（A 租户的 key 无法构造出访问 B 租户的请求）。
- **物理隔离**：数据目录 `<data_dir>/minisearch/<tenant>/<collection>/`（§5.2），租户删除 = 目录删除。
- **默认租户 `default`**：`--auth=off` 时所有请求视为 default 租户的 admin；`--auth=on` 后 default 与其他租户同等对待。

### 10.2 角色模型（四种静态角色，不做细粒度 RBAC）

| Role           | 能力                                                                     |
| -------------- | ------------------------------------------------------------------------ |
| `admin`        | 全局：租户 CRUD、所有租户的 key 签发/吊销、跨租户数据访问、全局 stats    |
| `tenant_admin` | 本租户：collection CRUD、本租户 key 签发/吊销（仅 writer/reader 角色）   |
| `writer`       | 本租户：documents upsert/delete + 检索；可被 `collections` 白名单收窄    |
| `reader`       | 本租户：search / get / analyze；可被 `collections` 白名单收窄            |

### 10.3 Key 签发与存储

- 格式：`msk_` + 32 字节 CSPRNG（base62，总长约 48 字符）。**服务端只存 sha256(key) + key_id + 元数据**（role、tenant、collections 白名单、created_at、revoked、last_used_ts）。
- 明文仅在签发响应中返回一次，之后不可找回（丢失只能吊销重签）。
- 存储：`<data_dir>/minisearch/auth/keys.json`（tmp + rename 原子写）；启动全量加载到内存索引 `sha256 → Principal`，吊销即时生效。
- **Bootstrap**：`--auth=on` 且 key store 为空时，首次启动生成 admin key，写入 `<data_dir>/bootstrap.key`（0600）并打印日志提示"妥善保存后删除该文件"。第二次启动检测到 key store 非空则不再生成。
- `last_used` 异步批量更新（离线刷盘），不阻塞请求路径。

### 10.4 认证中间件

- 位置：API Layer 第一环（路由分发之前），产出 `Principal{tenant, role, collections}` 注入请求上下文。
- 校验：提取 Bearer token → sha256 → 内存索引查找 → revoked 检查 → 角色与白名单判定。失败语义：无 token / 无效 token → `401`；token 有效但角色不足或 collection 越权 → `403`。
- `--auth` gflag：`off`（默认）= 单机模式，所有请求按 default 租户 admin 处理；`on` = 强制认证。
- **部署边界强制**：监听地址为非回环（非 127.0.0.1/::1）且 `--auth=off` 时，启动拒绝（fail-closed）。

### 10.5 租户与 Key 管理 API

端点清单见 §9.1。要点：

- 签发请求体：`{"role": "writer", "collections": ["loom-kb"], "ttl_days": 90}`（`collections` 为空/缺省 = 本租户全部；`ttl_days` 缺省 = 永久）。响应体一次性返回 `{"key_id": "k_...", "key": "msk_..."}`。
- 吊销：软删除（`revoked=true` 落盘），内存索引同步摘除。
- 租户删除：`?confirm=<tenant-name>` 显式确认；数据目录与该租户全部 key 一并删除，`admin` 专用。

### 10.6 配额（最小集）

| 配额                | 默认    | 检查点                              |
| ------------------- | ------- | ----------------------------------- |
| `max_collections`   | 32      | create collection                   |
| `max_docs`          | 1,000,000 | document upsert（按租户活跃 doc 计）|
| `max_disk_mb`       | 2,048   | checkpoint 完成后异步核对，超限告警 + 拒绝新写入 |

超出返回 `413` + 明细（哪个配额、当前值/上限）。配额随租户创建可调（admin）。不做：按 key 限流、QPS 配额（留 §16）。

### 10.7 审计（最小集）

每请求一行结构化日志：`ts, key_id, tenant, method, path, status, took_ms`（glog JSON / brpc builtin service 可查）。不做独立审计存储与检索（留 §16）。

### 10.8 里程碑归属

- **M0**：认证中间件、key/tenant 模型与存储、bootstrap、租户与 key 管理 API、`--auth` flag 与非回环 fail-closed。
- **M2**：collection 白名单收窄（writer/reader）、配额、审计日志。

## 11. Evaluation

任何 analyzer/词典/权重/模型变更必须过评测集回归。

### 11.1 金标准集

```
eval/golden/<collection>.jsonl
  {"query": "presto 查询卡住怎么排查", "expect": ["docs/presto-tuning.md#L23"], "notes": "多跳场景"}
```

- 起步 50 条（从真实使用中攒，loom KB 上线后每次人工纠正都沉淀为用例），目标 200 条。
- 覆盖类型：精确术语 / 概念改述 / 中英混合 / 2 字短词 / 多跳。

### 11.2 指标与脚本

- 指标：`Recall@5`、`MRR@10`、`nDCG@10`（expect 有序时）。
- `eval/eval.sh`：启动服务 → 灌入语料 → 逐条查询 → 计算指标 → 输出与基线的对比（标注回退 case）。功能回归由重写后的 `test.sh` 承担（§9.3）。

## 12. Performance Targets

| Metric                          | Target                     | Notes                                    |
| ------------------------------- | -------------------------- | ---------------------------------------- |
| search p99（不含 embedding/rerank，10 万 doc 内） | < 20 ms                   | BM25 内存倒排 + FAISS Flat               |
| search p99（embedding 缓存命中）  | < 30 ms                   |                                          |
| upsert（单条，含 server 端 embedding） | < 200 ms            | embedding 一次调用占主导                  |
| 冷启动加载（5 万 doc）            | < 5 s                     | checkpoint 反序列化                      |
| 常驻内存（5 万 doc × 1024 维）     | < 1 GB                    | FAISS Flat 4×n×d ≈ 200MB + 倒排 + docstore |

优化预留（不提前做）：FAISS 换 HNSW/IVF、f16 存储、posting 压缩、WAND、SIMD 分词。全部以"评测集效果不回退 + 目标未达标"为触发条件。

## 13. Module Renaming: recall → minisearch

命名对齐家族惯例（minidfs / minitable / minivessel）：`recall` 只描述了向量召回环节，新定位是通用混合检索服务，更名 **`minisearch`**。

迁移清单（git mv + 全局替换，一个提交完成）：

1. 目录：`cpp/pl/recall/` → `cpp/pl/minisearch/`；binary：`recall_server/recall_client` → `minisearch_server/minisearch_client`。
2. **MODULE.bazel**：`pip.parse` 的 `requirements_lock = "//cpp/pl/recall/embedding_server:..."` 路径同步修改（易漏点）。
3. proto：**全新定义** `minisearch.proto`（collection/document/search/auth message 与 v2 路由），不迁移 v4 的 `recall.proto` 消息定义。
4. CI 工作流与 `test.sh`、embedding_server/README.md 内的路径引用。
5. 本文档更名为模块主文档；`CLAUDE.md` 的目录注释同步。
6. `registry/`（本地 Bazel registry）无引用，无需变动。

`recall_client` 重写为 v2 API 示例（M0 交付物）。

## 14. Roadmap

| Milestone | Scope                                                                                          | 退出标准（可验证）                                        |
| --------- | ---------------------------------------------------------------------------------------------- | --------------------------------------------------------- |
| **M0** 基础重构（~2–3 个周末） | 目录改名；全新 v2 API（无 legacy 适配层，§9.3）；多 collection + schema；upsert/delete/tombstone；自动 checkpoint；`queries:analyze`；**认证中间件 + 租户/Key 管理 + bootstrap（§10.8）**；eval 骨架 | `test.sh` 重写为 v2 用例后全绿；collection CRUD 可用；重启零丢失；**认证用例通过（401/403/租户隔离/吊销生效）** |
| **M1** 中文 hybrid（~2–3 个周末） | jieba analyzer（Jieba-CPP，集成已完成）+ 内存倒排 + BM25 + ANN 并行 + RRF + 降级；rerank 钩子（Cohere 风格 API）；golden set 50 条 | loom KB 上线；eval：hybrid 的 Recall@5 显著高于 vector-only 与 BM25-only 基线 |
| **M2** 通用性 | docvalues + filter 下推；per-collection embedding + query 缓存；同义词/驼峰归一化；高亮；MCP server（含 `--api-key`）；**collection 白名单 + 配额 + 审计**；golden set 200 条 | loom + 至少一个非 KB 场景（如代码检索 collection）稳定运行 |
| **M3** 引擎化（**条件触发**） | segment/merge/manifest/WAL；HNSW 选项；posting 压缩；f16                                        | 触发条件（任一）：单 collection > 50 万 doc；checkpoint > 5s；并发写出现锁争用实测 |

M1 不引入 M3 架构。依据：目标写入频率为每天数十次 upsert（§2.2），M1 形态（单内存索引 + 高频 checkpoint）满足该负载。

## 15. Risks & Open Questions

| # | 风险/问题 | 缓解 |
| - | --------- | ---- |
| 1 | 词典运营是长期成本，中文效果一半在词典 | `queries:analyze` 工作台 + 热加载；每次人工纠正入 golden set |
| 2 | MLX BGE-M3 与 query/doc 不对称（instruction 前缀） | per-collection `query_prefix` 配置；评测集上 A/B 后定值 |
| 3 | 远端 embedding/rerank API 的可用性与延迟 | 本地 MLX 为默认兜底；降级矩阵保证 BM25-only 可用 |
| 4 | 分词上游风险：yanyiwu/cpp-jieba 仓库已删除（本风险已应验） | 已采用 ClickHouse/Jieba-CPP（commit 692f6001），本地 registry 模块 `jieba_cpp` 0.1.0 + `darts_clone` 0.9.0；C++23 `#embed` 由 genrule（bin2c）生成的 C 数组替代以兼容 C++20 编译矩阵；`analysis/jieba_test` 通过 |
| 5 | FAISS `remove` 成本（M1 upsert 需删旧向量） | M1 不调 FAISS remove：tombstone 过滤 + checkpoint 重建时物理清除 |
| 6 | scope creep（DSL/分布式/企业权限诱惑） | Non-Goals 写死；新想法一律先进 §16 待议清单 |
| 7 | 多 collection 常驻内存叠加 | collection 级 LRU 淘汰（闲置卸载、按需重载），M2 实现 |
| 8 | key 泄漏（明文只显一次，用户存明文于脚本/MCP 配置） | TTL 签发 + 即时吊销 + 审计行含 key_id 可追溯；泄露面与常规 API 服务等同 |
| 9 | 配额统计的准确性（M1 无 docvalues，租户活跃 doc 数需实时维护） | docstore 计数器随 upsert/delete/tombstone 原子增减，checkpoint 持久化 |

## 16. Future Enhancements（未排期）

继承 v4 Future 列表中仍有效项：brpc `/vars` Prometheus 指标（M1）、embedding server dynamic batching / 模型热切换、CORS。认证/租户方向：TLS 终结（公网部署前置）、审计日志独立存储与检索、per-key 限流与 QPS 配额、key 轮换 API。新增候选：段级 merge 工具、同义词挖掘辅助（基于共现统计）、collection 级快照导出/导入。

## Appendix A: v4 资产处置清单

| v4 组件 | v5 处置 |
| ------- | ------- |
| `FaissIndex`（IDMap 封装） | **复用**，包进 `index/vector/`，接口不变 |
| `IdMapper` | **演进**：per-collection 化，随 manifest 持久化 |
| `MetaStore` | **演进**：成为 docstore（stored fields + payload 存储） |
| `EmbeddingClient` / `OpenAIEmbeddingClient` | **复用**，per-collection 实例化 + LRU 缓存 |
| MLX embedding server | **原样保留**（目录随改名迁移，Bazel 集成不动） |
| `RecallHttpServiceImpl`（brpc + json2pb 模式） | **复用骨架模式**（`default_method` 路由 + json2pb 转换），handler 按 v2 API 全部重写 |
| snapshot save/load | **演进**为自动 checkpoint（manifest 化） |
| `test.sh`（19 用例） | **重写**为 v2 API 功能回归（v4 用例不保留）；评测职责移交 `eval/` |
| FAISS Bazel 集成（patch/SIMD/BLAS） | **原样复用** |
| gflags 启动参数 | 收敛：全局仅 `--port/--data_dir`，其余进 collection settings |
