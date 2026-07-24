# Shuttle：面向 Coding Agent 的工程知识检索服务设计

> `Shuttle`（梭子）在代码、文档与工程历史之间穿梭，为 `Loom`（织布机）持续送入可验证、可引用的知识。

| 字段 | 值 |
|---|---|
| 作者 | liubang (it.liubang@gmail.com) |
| 版本 | 0.1 |
| 状态 | Draft |
| 模块 | `cpp/pl/shuttle` |
| 核心语言 | C++20 |
| 服务协议 | gRPC + Protobuf |

## 1. 背景

通用 Coding Agent 可以读取当前工作区，却不了解用户跨仓库积累的设计文档、历史实现、故障经验和架构决策。仅靠 `grep`、LSP 或把大量文件直接放入模型上下文，会遇到跨数据源发现困难、上下文超限、历史信息丢失和证据不可复现等问题。

Shuttle 是独立于 Loom 的工程知识检索服务。它负责采集、规范化、切分、索引和检索代码及文档，返回带稳定来源、版本、可信度和快照信息的证据。Loom 仍负责 Agent Loop、工具权限、上下文预算、Prompt 构造和最终回答。

Shuttle 不是现有 `cpp/pl/recall` 的直接扩展。`recall` 是面向库表元数据的轻量向量召回服务；Shuttle 面向版本化工程知识，需要代码感知切分、稀疏与稠密混合检索、不可变快照、增量索引、引用溯源和更严格的恢复语义。底层 FAISS、Embedding Client 等成熟组件可在接口稳定后抽取复用，但两个服务不共享业务协议和持久化格式。

## 2. 目标与非目标

### 2.1 目标

1. 为 Loom 和其他客户端提供厂商无关、版本化的 gRPC 知识检索协议。
2. 首期支持本地 Git 仓库、代码文件和 Markdown/纯文本文档；Git 历史在核心检索稳定后按策略扩展。
3. 同时支持精确词法、路径、符号和向量检索，并通过混合召回与重排提高质量。
4. 每个结果都携带可验证引用：数据源、仓库、版本、路径、行号、内容哈希和索引快照。
5. 使用不可变 Segment 和 Manifest Snapshot 保证查询一致性、崩溃恢复与结果可解释性。
6. 支持内容哈希驱动的增量索引、逻辑删除、后台 Compaction 和快照回滚。
7. Embedding 与 Reranker 通过接口对接本地或远程模型服务，不绑定具体模型。
8. 默认本地优先、最小权限、数据不外发；远程调用具有明确出口策略。
9. 建立检索级和 Loom 任务级评测，避免只凭主观样例判断效果。
10. 在 macOS 与 Linux 上通过 Bazel 构建、测试和发布。

### 2.2 非目标

1. Shuttle 不生成最终自然语言答案，不承担 Agent 推理。
2. Shuttle 不替代 `grep`、`read_file`、LSP、Clang AST 或编译器；它是补充证据源。
3. 首版不实现通用网页爬虫、Office/PDF 全格式解析和企业级连接器市场。
4. 首版不实现多节点分布式一致性、跨地域复制和多租户 SaaS。
5. 首版不实现 Personal Memory 的提取、冲突、遗忘和生命周期管理；Memory 使用独立服务协议。
6. 不从零实现 Embedding 或 Cross-Encoder 模型训练框架。
7. 不允许客户端直接操作 FAISS、SQLite 表或内部 Segment 文件。

## 3. 设计原则

1. **证据而非答案**：服务返回原始证据、引用和排序解释，不返回不可审计结论。
2. **精确检索优先**：代码符号、路径、错误码和配置键优先使用词法/符号检索，向量检索用于语义补充。
3. **快照即查询边界**：一次请求只读取一个不可变快照，绝不混用新旧索引。
4. **发布而非原地修改**：索引更新生成新 Segment，验证后原子发布新 Manifest。
5. **内容与指令分离**：被索引资料是不可信数据，不能成为系统指令或提升 Loom 权限。
6. **版本是一等公民**：Chunker、Tokenizer、Embedding、Reranker、索引格式和协议均显式版本化。
7. **故障可判定**：写入和发布具有幂等键；无法判定结果时返回 `OUTCOME_UNKNOWN`，不得盲目重放。
8. **本地可用、远程可演进**：单机实现先满足个人使用，但协议和数据模型保留权限与多 Corpus 边界。
9. **质量由评测驱动**：索引和模型变更必须用版本化数据集比较 Recall、排序、引用和最终任务成功率。

## 4. 系统上下文与信任边界

```text
┌──────────────────────── Loom (Go) ─────────────────────────┐
│ Agent Loop │ Tool Runtime │ Context Manager │ Audit/Event  │
└───────────────────────────┬─────────────────────────────────┘
                            │ gRPC / UDS or mTLS
                            ▼
┌──────────────────── Shuttle Query Plane ────────────────────┐
│ Auth │ Search Orchestrator │ Hybrid Retrieval │ Rerank     │
│ Filters │ Pack Hints │ Citation │ Snapshot Reader          │
└───────────────────────────┬─────────────────────────────────┘
                            │ immutable snapshot
                            ▼
┌──────────────────── Shuttle Storage Plane ──────────────────┐
│ Catalog(SQLite WAL) │ Content Store │ Sparse/Dense Segments │
│ Manifest │ Task Journal │ Checkpoint │ GC                  │
└───────────────────────────▲─────────────────────────────────┘
                            │ publish
┌──────────────────── Shuttle Ingestion Plane ────────────────┐
│ Connectors │ Normalize │ Parse │ Chunk │ Embed │ Build     │
│ Diff │ Secret Policy │ Validate │ Atomic Publish           │
└───────────────────────────┬─────────────────────────────────┘
                            │ optional outbound calls
                  ┌─────────┴──────────┐
                  │ Embedding/Reranker │
                  │ local or remote    │
                  └────────────────────┘
```

信任边界如下：

- Loom 请求、仓库内容、文档内容和 Git 元数据均视为不可信输入。
- Query Plane 只读取已发布快照；Ingestion Plane 无权修改 Loom 工作区。
- Embedding/Reranker 是独立外部边界。正文是否允许外发由 Corpus 出口策略决定。
- Admin API 与 Query API 使用不同 Capability；查询权限不隐含索引或删除权限。
- 本地默认使用 Unix Domain Socket；TCP 默认仅监听 loopback。远程模式要求 mTLS。

## 5. 总体架构

### 5.1 Query Plane

在线查询路径保持只读：

```text
Authenticate
  → Validate and normalize query
  → Resolve one snapshot
  → Apply corpus/version/ACL filters
  → Sparse + Dense retrieval in parallel
  → Reciprocal Rank Fusion
  → deterministic boosts and deduplication
  → optional Cross-Encoder rerank
  → candidate grouping and pack hints
  → citations and diagnostics
```

Query Plane 不等待索引构建，不读取 staging 数据，也不在请求路径修改索引。Embedding 或 Reranker 不可用时，可按请求策略降级到 Sparse Search，并在响应中标记 `degraded`。

### 5.2 Ingestion Plane

索引任务异步执行：

```text
Discover source revision
  → enumerate and policy-filter documents
  → normalize and hash
  → compare with base snapshot
  → parse and chunk changed documents
  → batch embedding
  → build immutable sparse/dense segments
  → write tombstones
  → validate checksums and invariants
  → commit catalog metadata
  → atomically publish manifest
```

任务状态为：

```text
PENDING → RUNNING → BUILDING → VALIDATING → COMMITTING → COMPLETED
                  └──────────────→ FAILED
PENDING/RUNNING/BUILDING/VALIDATING → CANCELLING → CANCELLED
COMMITTING 中断 → RECOVERING → COMPLETED | FAILED | OUTCOME_UNKNOWN
```

### 5.3 Storage Plane

- **Catalog**：SQLite WAL，保存 Corpus、Source、Document、Chunk 元数据、任务、Manifest 和引用计数。
- **Content Store**：SHA-256 内容寻址目录，保存规范化原文、Chunk 正文和解析产物。
- **Sparse Segment**：每个 Segment 是独立、只读的 SQLite FTS5 数据库，包含词法、路径和符号索引；发布后不再写入。
- **Dense Segment**：FAISS 索引及向量 ID 映射。
- **Manifest**：描述一个可查询快照所引用的全部 Segment、版本和高水位。
- **Task Journal**：记录异步任务阶段和发布意图；与 Catalog 事务共同确定恢复边界。

SQLite 是控制元数据的事实来源；大文本和索引文件不放入 SQLite BLOB。Segment 和 Manifest 自身带格式版本、长度、SHA-256/CRC32C 校验。

## 6. 领域模型与稳定标识

### 6.1 核心对象

| 对象 | 含义 |
|---|---|
| `Principal` | 调用者身份及 Capability |
| `Corpus` | 权限、出口和索引策略一致的知识域 |
| `Source` | Git 仓库、目录或文档源 |
| `SourceRevision` | Git commit/tree、文档版本或扫描代次 |
| `Document` | 稳定 URI 对应的逻辑文档 |
| `DocumentRevision` | 文档某版本的规范化内容 |
| `Chunk` | 可检索且可引用的最小语义单元 |
| `Segment` | 一组不可变索引文件 |
| `SnapshotManifest` | 某一时刻完整、可查询的索引视图 |
| `IngestJob` | 可恢复、幂等的索引任务 |
| `Tombstone` | 在新快照中屏蔽旧文档或 Chunk 的逻辑删除记录 |

### 6.2 ID 规则

外部 ID 使用不透明 UUID/ULID；内容对象额外保存 SHA-256。不得用本机绝对路径作为仓库或文档的唯一身份。

- `corpus_id`：创建时分配，跨机器稳定。
- `source_id`：由 Corpus 内注册操作分配。
- `document_id`：`H(source_id, canonical_uri)`；重命名默认建模为删除加新增。
- `document_revision_id`：`H(document_id, normalized_content_hash, source_revision)`。
- `chunk_id`：`H(document_revision_id, chunker_id, chunker_version, kind, byte_range, normalized_chunk_hash)`。
- `segment_id`：Segment 内容哈希。
- `snapshot_id`：规范化 `ManifestPayload` 的 SHA-256；计算时排除 `snapshot_id`、文件 checksum、签名和 Catalog generation 等自描述/发布字段。

Chunk ID 包含 Chunker 版本，避免切分策略升级后错误复用；去重可另用 `normalized_chunk_hash`，不能混淆身份与内容相同。所有内容寻址 ID 只覆盖稳定业务负载，禁止把自身 ID 或校验字段纳入哈希形成自引用。Catalog generation 是单调发布序号，不参与 `snapshot_id`。

### 6.3 Chunk 类型

- `SYMBOL`：函数、方法、类、结构体、接口、枚举等完整符号。
- `CONTEXT`：符号附近注释、声明和有限上下文。
- `SECTION`：Markdown/文档标题下的语义段落。
- `FILE_SUMMARY`：文件级摘要；仅允许由版本化、可追踪的离线步骤产生。
- `COMMIT`：提交说明及关联变更摘要。
- `FALLBACK_WINDOW`：解析失败时的有重叠 Token 窗口。

每个 Chunk 保存 `path`、语言、标题路径、符号限定名、字节范围、行范围、父 Chunk、内容哈希、解析器版本和来源版本。

## 7. gRPC 协议

Proto package 建议使用 `pl.shuttle.v1`。字段只能追加，已发布 field number 永不复用；破坏性变更创建 `v2`。`request_id`、`trace_id` 通过 metadata 或请求字段传递；deadline 和取消从 gRPC context 获取。`effective_principal` 必须由 UDS peer credential、Token 或 mTLS 认证层解析，客户端自报字段最多作为审计 hint，绝不参与授权决策。服务端始终施加比客户端请求更严格的硬上限。

### 7.1 QueryService

```protobuf
service QueryService {
  rpc Search(SearchRequest) returns (SearchResponse);
  rpc GetChunk(GetChunkRequest) returns (GetChunkResponse);
  rpc BatchGetChunks(BatchGetChunksRequest) returns (BatchGetChunksResponse);
  rpc GetDocument(GetDocumentRequest) returns (GetDocumentResponse);
  rpc ExplainHit(ExplainHitRequest) returns (ExplainHitResponse);
}
```

`SearchRequest` 关键字段：

- 查询文本及可选精确标识符；
- Corpus/Source/仓库/路径/语言过滤器；
- `LATEST_CONSISTENT`、`EXACT_SNAPSHOT` 或最小 Source Revision；
- `SPARSE`、`DENSE`、`HYBRID` 检索模式；
- `top_k`、候选数和最大返回 Token/字节；
- 是否允许 Embedding/Reranker、是否允许降级；
- 当前仓库、Commit、编辑文件等非权威排序 Hint。

`SearchResponse` 关键字段：

- `snapshot_id`、Manifest generation、Source revision 和 `freshness_state`；
- `hits`：路径、行号、符号、分项分数、最终排序、来源和 trust label；正文使用统一的 `ContentMode = INLINE | REF | ELIDED`，并携带 `content_hash`、字节数和因策略省略时所需的 Capability；
- `pack_hints`：相邻关系、建议 merge group 和证据类型，不包含最终裁剪或 Prompt 顺序；
- `diagnostics`：使用的 Retriever、模型版本、候选数、截断、降级与阶段延迟；
- `next_page_token`；Token 绑定请求过滤器与 Snapshot，不能跨快照使用。

`ExplainHit` 返回可公开的排序组成和匹配词，不暴露模型私有推理。Shuttle 只产生不可变 Hit 与 Pack Hint；最终上下文排序、合并、裁剪、Token 预算和 Prompt Hash 由 Loom 独占负责。

### 7.2 CorpusAdminService

```protobuf
service CorpusAdminService {
  rpc CreateCorpus(CreateCorpusRequest) returns (CreateCorpusResponse);
  rpc RegisterSource(RegisterSourceRequest) returns (RegisterSourceResponse);
  rpc UpdateSourcePolicy(UpdateSourcePolicyRequest) returns (UpdateSourcePolicyResponse);
  rpc StartIngest(StartIngestRequest) returns (StartIngestResponse);
  rpc GetJob(GetJobRequest) returns (GetJobResponse);
  rpc CancelJob(CancelJobRequest) returns (CancelJobResponse);
  rpc ListSnapshots(ListSnapshotsRequest) returns (ListSnapshotsResponse);
  rpc PinSnapshot(PinSnapshotRequest) returns (PinSnapshotResponse);
  rpc UnpinSnapshot(UnpinSnapshotRequest) returns (UnpinSnapshotResponse);
  rpc ActivateSnapshot(ActivateSnapshotRequest) returns (ActivateSnapshotResponse);
  rpc DeleteSource(DeleteSourceRequest) returns (DeleteSourceResponse);
}
```

`StartIngest` 接受 `idempotency_key` 和 `base_snapshot_id`。`idempotency_key → job_id` 映射与 Job 状态持久化在 Catalog；重复键返回同一 Job，不重复构建。正常发布仅当 `current_active_snapshot == job.base_snapshot_id` 时 CAS 成功，否则返回 `SNAPSHOT_CONFLICT/REBASE_REQUIRED`，不得覆盖并发更新。首版按 Corpus 线性化发布，不自动合并两个基于同一基线构建的完整视图。

`ActivateSnapshot` 使用 `expected_active_snapshot` 乐观锁，防止旧任务覆盖更新版本。正常 Ingest 在发布事务内自动激活新 Snapshot；该 RPC 仅用于将已验证且仍被保留的 Snapshot 手动回滚/前滚，不是正常 Ingest 的必需步骤。

### 7.3 MetaService

```protobuf
service MetaService {
  rpc Health(HealthRequest) returns (HealthResponse);
  rpc Ready(ReadyRequest) returns (ReadyResponse);
  rpc GetCapabilities(GetCapabilitiesRequest) returns (GetCapabilitiesResponse);
  rpc GetStats(GetStatsRequest) returns (GetStatsResponse);
}
```

Capability 至少公布协议版本、索引格式、Chunk 类型、Retriever、最大请求限制、模型 ID、认证模式和是否支持 Snapshot Pin。

### 7.4 错误模型

使用 gRPC canonical status，并在 details 中附带稳定领域错误：

- `INVALID_ARGUMENT`
- `UNAUTHENTICATED` / `PERMISSION_DENIED`
- `CORPUS_NOT_FOUND` / `SNAPSHOT_NOT_FOUND`
- `SNAPSHOT_CONFLICT`
- `SOURCE_STALE`
- `MODEL_UNAVAILABLE`
- `RESOURCE_EXHAUSTED`
- `DEADLINE_EXCEEDED` / `CANCELLED`
- `CORRUPT_INDEX`
- `OUTCOME_UNKNOWN`
- `INTERNAL`

错误 detail 包含安全消息、`retryable`、建议退避、操作 ID 和状态查询方式，不返回本地秘密路径、凭证或未过滤正文。关键错误映射固定如下：

| Shuttle 错误 | Loom 语义与动作 |
|---|---|
| `MODEL_UNAVAILABLE` | `unavailable`；允许降级时使用 Sparse/local，否则按退避重试只读请求 |
| `SOURCE_STALE` | `stale_data`；不得重试同一 Snapshot，优先本地 `read_file` |
| `SNAPSHOT_CONFLICT` | `conflict`；Admin 操作必须 rebase，只读查询仅在未要求精确 Snapshot 时可重取 active |
| `CORRUPT_INDEX` | `unavailable/internal`；标记 Corpus unhealthy，不静默切换到语义未知的 Snapshot |
| `OUTCOME_UNKNOWN` | Admin 操作挂起并调用 `GetJob` 对账，禁止自动重放 |

`GetJob` 至少返回 Job 状态、base/candidate Snapshot、观察到的 active Snapshot、最后一次 Reconcile 结论和 `safe_retry`。只有 Catalog、Job Journal 与对象存储证据均不足以判定时才使用 `OUTCOME_UNKNOWN`。

## 8. 数据采集、规范化与切分

### 8.1 Connector

首批 Connector：

1. `GitConnector`：索引已提交 tree，revision 为 Commit/Tree Hash。
2. `DirectoryConnector`：索引显式允许的本地目录，revision 为扫描代次及内容 Merkle Root。

Phase 4 再提供默认关闭的 `GitHistoryConnector`，按 Corpus Policy 索引 Commit Message、变更路径和有限 Diff 摘要，避免历史内容在基础排序尚未稳定时制造重复和过期证据。

Connector 只产生标准 `SourceDocument`，不直接写索引。它必须遵守 ignore 规则、文件数/大小/深度上限，不跟随越界符号链接，不读取特殊文件。

### 8.2 规范化

- 检测二进制和文本编码，首版仅接受 UTF-8 或可安全转换的文本。
- 换行统一为 LF，但引用同时保留原始行映射。
- 不改变代码语义所需空白；用于哈希的规范化规则必须版本化。
- 记录 MIME、语言、内容长度、内容哈希和原始来源。
- 默认忽略构建产物、依赖目录、VCS 内部目录、密钥和超大生成文件。

### 8.3 代码切分

优先使用 Tree-sitter 等版本化 Parser 按语义单元切分，后续可接入 Clang AST/LSP。过大的符号按语句或 Token 窗口二次切分，并保留父符号签名；解析失败时回退到固定 Token 窗口。禁止静默丢弃失败文件，失败原因进入任务报告。

### 8.4 文档切分

Markdown 按标题树、段落、列表、表格和代码块切分。Chunk 携带完整标题路径；代码块不从其解释段落中无条件拆离。超长 Section 再按 Token 上限切分并保留少量重叠。

### 8.5 Secret 与分类策略

索引前执行路径规则、文件类型规则和 Secret Scanner。检测到高置信秘密时默认拒绝该文档并产生安全审计记录，不能将秘密作为日志或错误正文。Corpus 具有：

- 允许/拒绝路径；
- 数据分类；
- 是否允许发送到远程 Embedding/Reranker；
- 是否允许返回完整正文；
- 保留和删除周期。

## 9. Embedding 与 Reranker

### 9.1 Provider 接口

C++ 领域接口与传输实现分离：

```cpp
class EmbeddingProvider {
public:
    virtual ~EmbeddingProvider() = default;
    virtual StatusOr<EmbeddingBatch> EmbedDocuments(
        std::span<const EmbeddingInput> inputs) = 0;
    virtual StatusOr<EmbeddingVector> EmbedQuery(
        const EmbeddingInput& input) = 0;
};

class RerankProvider {
public:
    virtual ~RerankProvider() = default;
    virtual StatusOr<RerankResult> Rerank(
        const RerankRequest& request) = 0;
};
```

可实现 OpenAI-compatible HTTP、本地 gRPC、ONNX Runtime 或已有 MLX 服务 Adapter。凭证通过 Secret Reference 注入，不持久化明文。

### 9.2 模型身份

模型身份包含：provider、model、revision、dimension、distance metric、normalization、query/document template、tokenizer 和最大输入长度。Dense Segment 必须绑定完整身份；不同身份的向量禁止混合。

### 9.3 批处理、重试与降级

- 文档 Embedding 使用有界动态 Batch，任务 Checkpoint 保存已完成批次。
- Query Embedding 使用短 deadline 和有界缓存。
- 仅对可判定未执行或带幂等语义的请求自动重试。
- Dense 不可用时按客户端策略降级到 Sparse；响应明确标记，不能伪装为完整 Hybrid。
- Reranker 只处理有限候选，失败后回退到确定性融合排序。

## 10. 检索与排序

### 10.1 Sparse Retrieval

Sparse Index 至少支持：

- BM25 文本检索；
- 原始标识符精确匹配；
- CamelCase、snake_case 和路径分词；
- 符号限定名与标题路径；
- 仓库、Commit、语言、路径和 Chunk 类型过滤。

首版确定使用**不可变 FTS5 Segment**：每次增量任务创建新的 SQLite 数据库文件，完成写入后执行 `optimize`、关闭 Writer、计算校验和并以只读 immutable URI 打开。Manifest 引用一组 FTS Segment 和 Snapshot-scoped Tombstone；查询对该集合并行检索、过滤删除项并融合排名。旧 Snapshot 始终引用原 Segment 集，因此不会被新发布修改。Compaction 读取一组旧 Segment，生成语义等价的新 Segment/Tombstone 集，只有新 Manifest 激活且旧对象不再被任何 active/pinned/retained Snapshot 引用后才能回收旧文件。

FTS5 `bm25()` 仅用于单 Segment 内排序，因为不同 Segment 的文档频率和长度统计不可直接比较。`SparseSegmentCoordinator` 为每个 Segment 分配有界候选预算，先应用 Tombstone/ACL 过滤，再使用 rank-based RRF 合并各 Segment 排名；精确路径和限定符号属于独立 protected channel，高于普通 BM25。首版不承诺全局 BM25；若以后需要，必须引入 Snapshot 级全局词法统计。

Query Plane 通过 `SparseIndex` 抽象访问，API 不暴露 FTS5 格式。若容量评测证明多 Segment fan-out 或 FTS5 并发无法达标，可替换 Segment 实现，但必须保持相同的 immutable/pin/manifest 契约。禁止使用共享可变 FTS 库加 generation filter 冒充快照隔离。Phase 0 必须通过 Bazel Smoke Test 和启动探针实际执行 `CREATE VIRTUAL TABLE ... USING fts5`；缺少 FTS5 时 fail fast，不允许静默退化。

### 10.2 Dense Retrieval

FAISS 作为首选向量引擎。Dense 与 Sparse 一样采用不可变 Segment：增量更新只新增 Segment，删除仅由 Snapshot Tombstone 屏蔽，禁止依赖 FAISS 原地 `remove_ids` 获得快照语义。Compaction/Rebuild 才物理删除向量并生成新 Segment。

- 小于约十万 Chunk 默认归一化向量加 `IndexFlatIP`；
- 更大数据量通过评测选择 HNSW 或 IVF；
- 训练型索引只在 Build/Compaction 阶段训练，training artifact、样本身份、ANN 参数和模型身份进入 Manifest；
- `chunk_id → dense_internal_id` 映射和 Tombstone identity 属于 Segment/Snapshot 元数据；
- ANN 参数属于 Snapshot 配置并进入评测报告；
- FAISS 内部 ID 不能作为外部 Chunk ID。

### 10.3 融合与重排

Sparse 和 Dense 候选并行召回，使用 RRF 融合：

\[
\operatorname{RRF}(d)=\sum_{r}\frac{w_r}{k+\operatorname{rank}_r(d)}
\]

然后应用确定性 Boost：精确符号、路径、当前仓库、精确 revision 和当前文件；Boost 参数版本化。候选先按 Chunk/内容哈希去重，再可选 Cross-Encoder 重排。不得直接相加未经校准的 BM25 与向量原始分数。

Reranker 只接收已经 ACL/分类过滤且按模型上限截断的候选，默认 `top_n <= 32` 并受总 Token、字节和 deadline 硬限制。`EXACT_PATH`、`EXACT_SYMBOL`、`CURRENT_FILE_EXACT` 属于 protected tier，模型只能在同 tier 内重排，不能把确定性精确命中降到普通语义结果之后。稳定 tie-break 顺序为：protected tier、融合 rank、确定性 boost、精确 source revision、新鲜度、`chunk_id`。

### 10.4 Candidate Grouping 与 Pack Hint

Shuttle 不拥有最终 Packing。它只对候选生成可忽略的组合提示：

- 只有 `snapshot_id`、`document_revision_id`、ACL 和 classification 全部相同的相邻 Chunk 才可建议合并；
- 同一文件和 Source 的建议结果带 diversity 分组，便于 Loom 限额；
- `SYMBOL`、`SECTION`、`COMMIT`、`FILE_SUMMARY` 分 lane，默认不跨 lane 合并；
- 建议保留符号 signature、leading comments、标题及最小必要正文；
- 默认抑制同一文档的旧版本，除非请求明确比较历史；
- Hint 必须保留原始 Chunk 引用和边界，不能生成无法追溯的新正文。

Loom 负责最终合并、裁剪、Token 预算、注入顺序和 Prompt Hash，因此 Shuttle 的 Hint 变化不会改变已持久化模型请求的恢复语义。

## 11. 快照、增量更新与并发

### 11.1 不可变 Segment

构建任务只写唯一 staging 目录。所有对象统一执行 `write temp → fsync(file) → rename → fsync(parent directory)`；若目标 hash 已存在，则校验类型、长度和哈希后复用，绝不覆盖。逻辑引用只记录在 Catalog，对象文件不自带可变 refcount。已发布 Segment 永不原地修改。

Manifest 至少包含：

```text
format_version
snapshot_id（由 canonical payload 派生，不进入 payload）
catalog_generation（仅 Catalog 发布序号，不参与 snapshot_id）
corpus_id
base_snapshot_id
source revisions
schema / parser / chunker / tokenizer versions
embedding and reranker identities
sparse/dense segment identities and checksums
tombstone sets
created_at
catalog high watermark
file checksum（序列化时该字段置零，不参与 snapshot_id）
```

### 11.2 发布事务

唯一对外可见的发布事实来源是 SQLite Catalog 的 `active_snapshot_id`；Manifest、Segment 和 Content 文件只是不可变被引用对象，单独存在不代表已发布。事务步骤：

1. 在 Catalog 创建带 writer epoch、idempotency key、base Snapshot 和 staging identity 的 Job；
2. 构建并持久化所有 Content、Sparse/Dense Segment 和 Tombstone 对象；
3. 验证文件校验和、ID 唯一性、向量维度和引用完整性；
4. 对 canonical `ManifestPayload` 计算 `snapshot_id`，安装 immutable Manifest 对象并同步父目录；
5. SQLite 事务记录 candidate Manifest、对象引用和 publish intent；
6. SQLite 事务执行 `active_snapshot_id == job.base_snapshot_id && writer_epoch == job.writer_epoch` 的 CAS；CAS 成功即构成发布完成，并在同一事务记录 Job `COMPLETED`；
7. 响应 Job 完成；任何派生日志或 Metrics 可在之后补记。

崩溃判定矩阵：

- 对象/Manifest 未安装且 active 未变：未发布，可清理 staging；
- Manifest 已安装但 active 未变：未发布，对象作为候选保留到安全回收期；
- active 已 CAS 到 candidate：已发布，即使响应或派生日志缺失；
- active 变为其他 Snapshot：发布冲突，必须 rebase；
- 只有 Catalog/Journal/对象证据损坏到无法证明上述任一状态时才标记 `OUTCOME_UNKNOWN`。

Job Journal 至少记录 `base_snapshot_id`、`candidate_snapshot_id`、writer epoch、对象安装状态、CAS 是否尝试/成功和最后 Reconcile 结果。客户端超时后必须先 `GetJob`，不得直接重复发布。

### 11.3 查询并发

请求开始时从 Catalog 解析并 pin 一个 Snapshot Handle，请求结束释放。新 Manifest 激活不影响旧请求。优先级固定为 `pin/retention > snapshot reachability > delete request > GC efficiency`。Tombstone 是 Snapshot-scoped immutable object，只影响引用它的 Snapshot；旧 pinned Snapshot 仍可看见后来被删除的内容。Compaction 必须生成语义等价视图，GC 只能删除不被 active snapshot、显式 pin、运行中查询、构建/备份任务或保留策略引用的对象。

### 11.4 增量更新

以基线 Snapshot 和 Source Revision 做 Diff：未变文档复用 Chunk/Segment 引用；新增或修改文档生成新 Chunk；删除文档写 Tombstone。首版允许增量构建小 Segment，达到数量或删除比例阈值后后台 Compaction 合并。

### 11.5 脏工作区

首版明确提供两种语义：

- `COMMITTED_ONLY`：仅索引 Git Commit，最稳定；
- `SCANNED_WORKTREE`：索引某次扫描的工作树 Merkle Root，并返回扫描时间和 freshness。

Shuttle 不假装持续追踪每个未提交编辑。Loom 对当前文件仍优先使用本地 `read_file/search_text/LSP`；后续可增加 Run 级内存 Overlay Index。

## 12. 崩溃恢复、备份与 GC

启动顺序：

1. 获取 OS 排他锁，并在 Catalog 领取单调递增的 `writer_epoch`；所有 Job、对象安装意图和 active CAS 都携带并校验该 epoch，旧 Writer 即使仍存活也不能提交；
2. 打开 SQLite 并执行兼容迁移；
3. 对 active Manifest 做同步轻量校验：格式、Manifest 校验和、Segment 存在性/长度、文件身份、Catalog 引用，并实际只读打开每个 FTS5 Segment、读取 FAISS header/维度/ID map 元数据及 Tombstone header；
4. 重放未完成 Job Journal；
5. reconcile `COMMITTING` 操作；
6. 只有轻量校验和 Reconcile 成功后才 Ready；否则回退到最后一个已知健康 Snapshot 或 fail closed；
7. Ready 后异步执行全文件 checksum、索引可读性和引用遍历 Scrub，发现损坏立即摘除受影响 Snapshot、告警并回退；
8. 回收超时 staging 文件和确定无引用的 orphan。

轻量启动校验服务于冷启动目标，完整 Scrub 服务于静默损坏发现；两者不能以同一个“校验”指标验收。

只读副本可共享已发布对象，但不得写 Catalog、Journal、active pointer 或对象目录。Checkpoint 只是可从 Catalog 与不可变对象重建的派生加速状态，可缓存最近健康 Snapshot、Scrub 进度和 GC 游标；`idempotency_key → job_id` 等去重状态必须以 Catalog 为准，Checkpoint 缺失或损坏只能影响恢复速度，不能改变恢复结果。

备份开始时在 Catalog 一致读视图中选择 root Snapshots 并创建临时 Pin，生成包含 Catalog high watermark、Journal cutoff 和全部可达对象哈希的 `backup_manifest`；随后复制 SQLite 一致快照及对象闭包。恢复时先恢复 Catalog，再校验 `backup_manifest` 的完整对象闭包，通过后才能 Ready。Checkpoint 不等于 Backup，也不能单独作为备份产物。

GC 采用 mark-and-sweep：从 active/pinned/retained Snapshot 和运行任务标记可达对象，再经过最小安全期后删除。删除 Source 先发布 Tombstone Snapshot，再按保留策略物理清除；提供可验证的数据删除报告。

## 13. 安全与隐私

### 13.1 主要威胁

- 路径穿越、符号链接逃逸和特殊文件读取；
- 仓库中的 Prompt Injection 或恶意超长内容；
- Secret/PII 被索引、记录或发送到远程模型；
- Query 与 Admin 权限混淆；
- Corpus 间缓存污染和越权召回；
- 恶意 Protobuf、压缩炸弹、超大候选集导致资源耗尽；
- 被篡改 Segment、Manifest 或模型响应；
- 日志、Trace、备份和评测夹具泄露正文。

### 13.2 控制措施

- 路径 canonicalize、根目录约束、`openat`/平台等价安全打开、符号链接策略和文件上限；
- Corpus Capability：`query`、`ingest`、`manage`、`export_content`、`remote_inference`；强制映射为 `Search(REF/ELIDED)=query`、`Search(INLINE)=query+export_content`、`GetChunk/GetDocument=query+export_content`、使用远程模型的检索或索引额外要求 `remote_inference`、写入/激活/删除要求 `ingest/manage`；
- 查询结果标记为 `UNTRUSTED_EVIDENCE`，Loom 不把其内容当作指令；
- 索引前 Secret Scanner，出口前再次应用分类策略；
- 本地 UDS 文件权限或短期 Token，远程 mTLS 和短期受众限定凭证；
- 请求/响应、并发、内存、CPU、Token 和文件数量硬限制；
- Artifact 校验和、原子安装、只读权限和启动 Scrub；
- 日志默认只保存 ID、哈希、大小、状态和延迟，不保存查询全文和 Chunk 正文。

## 14. Loom 集成

以下是 Loom–Shuttle 集成契约草案，依赖 Loom 后续扩展知识引用、事件和 Context Manifest；当前实现尚不具备这些字段。v1 的 Loom Run 只接入只读 `QueryService`，`CorpusAdminService` 由独立 CLI/Daemon 调用，不进入 Run 恢复路径。Loom 通过 `KnowledgeService` 领域接口依赖 Shuttle，gRPC 只是 Adapter。建议在 Loom 暴露：

- 模型主动工具：`knowledge_search`、`knowledge_get`；
- Context Manager 自动检索：默认仅高置信、小预算、可关闭；
- Knowledge 结果统一转换为带 Source、Trust、TokenCount 的 `ContextItem`。

一次检索在 Loom Event Store 中记录两层事实：

1. 检索事实：请求 ID、脱敏查询哈希、Corpus/过滤器、Snapshot ID、返回 Chunk IDs、排序与降级；
2. 上下文配方：有序 `KnowledgeContextRef[]`，每项包含 Corpus/Snapshot/Chunk/Content Hash、实际注入顺序、merge group、裁剪范围、Trust、Tokenizer、预算桶和压缩/截断记录。

Loom 必须扩展 `ContextManifest` 以持久化上述引用，并区分 `input_material_hash` 与真正序列化后的 `final_provider_request_hash`；在 Provider Adapter 尚不能提供后者前，不得声称字节级重放。正文是否进入 Loom Artifact Store 由数据策略决定；若策略禁止持久化正文，恢复只能重用允许持久化的派生内容，或明确重新构造新一轮请求。

未来若允许 Loom Run 发起 Admin 操作，前置条件是新增 `knowledge.admin_requested/progressed/reconciled` 事件和 `Checkpoint.pending_external_ops[]`，持久化 `job_id`、`idempotency_key`、base/candidate Snapshot 与最后远端状态。出现 `OUTCOME_UNKNOWN` 时 Run 以该原因挂起并先调用 `GetJob` 对账，绝不自动重放 Admin 写入。Loom 传给 Shuttle 的授权来自认证层生成的 Corpus-scoped delegation，不来自模型可控 Tool 参数。

降级顺序：

```text
Shuttle Hybrid
  → Shuttle Sparse-only（模型服务不可用）
  → Loom 本地 search_text/LSP/read_file（Shuttle 不可用或内容过期）
```

对于当前工作树与 Shuttle Snapshot 不一致的文件，Loom 本地事实优先。Shuttle 返回的引用必须允许 Loom 在权限允许时重新读取并验证内容哈希。

## 15. 配置与部署

配置优先级：编译默认值 < 配置文件 < 环境变量 < CLI；秘密只通过引用注入。配置分为：

- Server：UDS/TCP、线程、deadline 和消息上限；
- Data：数据目录、SQLite、对象目录、保留策略；
- Corpus Policy：路径、分类、远程出口和模型；
- Retrieval：候选数、RRF、Boost、Rerank 和 Pack 预算；
- Ingestion：并发、Batch、文件和 Chunk 限制；
- Observability：日志、Metrics、Trace 内容策略。

部署形态：

1. **Local sidecar（默认）**：Loom 与 Shuttle 同机，通过 UDS；Embedding 可本地运行。
2. **Local daemon**：多个 Loom 客户端共享查询服务，OS 锁加 Catalog `writer_epoch` fencing 管理唯一 Writer。
3. **Remote service（后续）**：mTLS、独立身份、Corpus ACL 和严格出口策略。

关闭流程停止接收 Admin 写入、取消或 Checkpoint 构建任务、等待查询释放 Snapshot Handle、同步 Catalog 后退出。超过宽限期则保留可恢复 Journal，不强行宣称提交成功。

## 16. 可观测性

结构化日志字段包括：`request_id`、`trace_id`、`principal_id`、`corpus_id`、`job_id`、`snapshot_id`、`source_revision`、`retriever`、`status`、`degraded`、耗时和大小；默认不记录正文。

Metrics：

- Query QPS、P50/P95/P99 和错误率；
- Sparse/Dense/Rerank/Pack 分阶段延迟；
- Recall 候选量、去重率、缓存命中率和降级率；
- 索引文档/Chunk 数、吞吐、失败率、发布延迟和 ingest lag；
- Active/pinned Snapshot、Segment 数、Compaction backlog 和 orphan 数；
- Embedding/Reranker 延迟、Batch 大小、限流和错误；
- 恢复耗时、校验失败、回退和 `OUTCOME_UNKNOWN` 次数；
- 被策略拒绝的秘密和越权请求计数（不含秘密内容）；
- 磁盘剩余空间、staging/segment/backup 用量、空间放大率和 `ENOSPC` 次数。

Trace：

```text
Search → resolve_snapshot → sparse/dense → fusion → rerank → pack
Ingest → discover → normalize → parse → chunk → embed → build → validate → publish
```

OTel 导出默认关闭且异步；观测后端失败不能影响查询和发布。

## 17. 性能与容量目标

首个生产基线按单用户本地服务设定：

- 单实例支持 100 个 Corpus、合计 100 万个 Chunk、Embedding 维度不高于 1536；
- 缓存命中时 Sparse Search P95 < 50ms；
- 不含外部模型时 Hybrid Search P95 < 150ms；
- 本地 Rerank 的完整查询目标 P95 < 500ms；
- Query Plane 可接受 16 个并发查询，索引期间查询延迟退化不超过 50%；
- 10 万未变文件的增量发现目标 < 10s，实际索引耗时由变更量和模型决定；
- 有 Checkpoint 的百万 Chunk 冷启动 Ready 目标 < 30s；
- 已确认完成的发布 RPO 为 0；单机故障恢复 RTO 目标 < 5min；
- 默认单次返回不超过 64 个 Hit、2MiB 正文和配置的 Token 上限。

正确性、恢复和安全目标必须在 macOS/Linux 都满足。性能目标只在版本化基准环境中作为硬门槛，需记录 CPU、内存、磁盘、OS、编译器、Release 配置、FAISS index/线程数和 Segment 数；当前 macOS FAISS 使用 OpenMP stub 时为单线程基线，不与 Linux 多线程发布基线混用。目标可通过基准前置 ADR 调整，但不能以牺牲 Recall、Snapshot 一致性或安全为代价。

索引、Compaction 和备份在启动前估算峰值 scratch space，并预留“当前可用快照 + 回滚快照 + staging”的安全空间。低于配置的 hard watermark 时拒绝新写任务但继续只读查询。写 staging/Manifest 失败可终止当前任务；对象 rename 成功但父目录 `fsync` 失败、或 Catalog commit/CAS 状态不可判定时必须先 Reconcile。只有已脱离 Job 候选集且不可由 active/pinned/retained/backup/running-job 触达的对象才能作为 orphan 清理。Compaction、Backup、Checkpoint 遇到 `ENOSPC` 只失败当前维护任务，不触碰 active/pinned 数据。Soft watermark 触发告警，但 GC 仍遵守引用和安全期。

## 18. 测试与评测

### 18.1 自动化测试

- 单元测试：ID、规范化、Chunk 边界、Tokenizer、RRF、过滤、Packing、Manifest 和状态机；
- Parser Golden Test：多语言符号、Markdown、Unicode、长文件和解析失败；
- 协议契约：Go/C++ 客户端兼容、未知字段、错误 detail、deadline 和取消；
- 集成测试：全量/增量索引、删除/重命名、快照 Pin、并发查询和 Compaction；
- 故障注入：Segment finalize、Catalog commit、Manifest install、active CAS 和 Checkpoint 各阶段崩溃；
- 数据损坏：缺失/篡改 Segment、坏 Manifest、重复 Journal 和 SQLite 恢复；
- 安全测试：路径穿越、符号链接竞态、Secret、Prompt Injection、越权、资源耗尽和缓存隔离；
- 跨平台测试：macOS Apple Clang 与 Linux GCC/Clang，ASan 默认开启。

### 18.2 Retrieval Eval

从真实工作任务建立版本化评测集，每条包含查询、指定 Snapshot、相关 Chunk、禁止的过期结果和期望引用。覆盖符号定位、相似实现、设计依据、历史修复、构建方法和跨仓库知识。

指标：

- Recall@K、MRR、nDCG@K；
- Exact Path/Identifier Success；Phase 2 起增加 AST Symbol Success；
- Citation Precision 与行号准确率；
- stale-hit、wrong-version 和 irrelevant-context 比例；
- P50/P95 延迟、峰值内存和单查询模型成本。

### 18.3 Loom 端到端 Eval

A/B 比较无 Shuttle、Sparse-only、Hybrid 和 Hybrid+Rerank：任务 Pass@1、验证通过率、Token/时间、无关修改、安全违规和引用正确率。必须区分召回失败、排序失败、上下文选择失败和生成失败。

## 19. Bazel 与目录规划

```text
cpp/pl/shuttle/
├── DESIGN.md
├── BUILD
├── api/                 # gRPC service adapters
├── server/              # main、配置和生命周期
├── catalog/             # SQLite schema、transaction 和 migration
├── connector/           # Git、directory、history
├── document/            # 规范化领域模型
├── parser/              # 代码与文档 parser
├── chunker/             # semantic/fallback chunking
├── embedding/           # provider abstraction/adapters
├── tokenizer/           # identifier/path/model tokenization
├── retrieval/
│   ├── sparse/          # FTS segments、coordinator、rank merger
│   ├── dense/           # FAISS segments、training artifacts
│   └── hybrid/
├── rerank/
├── packing/             # pack hints；final packing 在 Loom
├── index/               # segment、manifest、snapshot、compaction
├── storage/             # content store、journal、checkpoint、GC
├── secure_fs/           # fd-based safe walk 与原子对象安装
├── security/            # auth、policy、classification
├── observability/
├── benchmark/
└── eval/

proto/shuttle/v1/
├── common.proto
├── query.proto
├── admin.proto
├── meta.proto
└── BUILD
```

Proto 放在共享 `proto/` 下，便于 Go Loom 与 C++ Shuttle 生成 Stub。所有 C++ Target 使用仓库统一的 `COPTS`、`LINKOPTS` 和 `TEST_COPTS`。首期优先复用已有 Abseil、gRPC、Protobuf、SQLite、FAISS、OpenSSL/哈希和 GoogleTest 依赖；新增解析依赖前需单独评估许可证、平台和 Bazel 维护成本。

## 20. 分阶段路线

### Phase 0：协议与一致性骨架

范围：领域模型、Proto v1、Fake Retriever/Provider、Catalog Schema、Job 状态机、内容寻址 Store、Manifest/Snapshot、writer epoch/fencing、安全路径基座、FTS5 Bazel/启动能力探针和 C++/Go 契约测试。

验收：同一 Snapshot 查询确定；发布故障点恢复不出现半快照；重复幂等键不重复提交；Loom Fake Client 能检索、引用和降级。

### Phase 1：Sparse 可用闭环

范围：Git/Directory Connector、规范化、Markdown 与 fallback Chunker、identifier/path Tokenizer 和基础限定名提取、不可变 FTS Segment、Sparse Segment Coordinator、增量 Diff、Tombstone、Query/Admin Server、引用和基础 Metrics。完整 AST Symbol Chunking 不属于本阶段。

验收：至少 200 条版本化查询集上 Exact Path/Identifier Success >= 95%、Sparse Recall@10 >= 80%、Citation Precision >= 98%、wrong-version/stale-hit <= 1%；删除内容在新 Snapshot 中召回率为 0；查询结果可按路径、行号和哈希验证。完整 Symbol 指标从 Phase 2 Parser 落地后开始统计。

### Phase 2：代码感知与 Hybrid Retrieval

范围：优先为 C++ 与 Go 引入代码 Parser（其他语言按评测扩展）、不可变 FAISS Segment、Embedding HTTP/gRPC Adapter、RRF、确定性 Boost、Pack Hint、缓存和模型降级。本阶段不要求内嵌 ONNX/MLX Runtime。

验收：同一至少 200 条版本化查询集上 Hybrid Recall@10 >= 90%、MRR >= 0.75、Citation Precision >= 98%，且 Recall@10 相对 Sparse 基线绝对提升 >= 5 个百分点；至少 50 个重复运行的 Loom 任务上 Pass@1 不低于 Sparse 基线且有统计报告。模型不可用时服务可用且明确降级。阈值调整必须通过版本化 ADR，不能在发布评测后临时放宽。

### Phase 3：生产可靠性

范围：Checkpoint、Reconciler、Compaction、GC、Scrub、备份恢复、Secret/分类策略、认证、审计、OTel 和完整故障注入。

验收：所有规定崩溃点不丢已提交快照、不暴露半成品、不误删 Pin 对象；备份恢复和数据删除经过演练；安全回归无高危失败。

### Phase 4：质量与数据源扩展

范围：Cross-Encoder、Git History、PR/Issue/Wiki Adapter、查询改写、文件摘要、Run Overlay Index 和多 Corpus 联合检索。

验收：每项能力先通过离线与 Loom A/B 评测再默认启用；旧协议客户端继续工作。

### Phase 5：远程与规模化（按需）

范围：远程 mTLS、多租户隔离、配额、对象存储、读副本和分片。只有个人单机容量或共享需求明确超出时才进入，不预先引入分布式共识。

## 21. 1.0 发布门槛

1. Query/Admin/Meta gRPC v1 通过 C++ 与 Go 契约测试。
2. Git、Markdown 和代码数据形成全量与增量索引闭环。
3. Sparse + Dense + RRF 可用，Embedding/Reranker 故障可安全降级。
4. 每个 Hit 都有 Snapshot、Source Revision、路径、行号和内容哈希。
5. 不可变 Segment、Manifest 原子发布、Pin、Tombstone、Compaction 和 GC 通过并发测试。
6. 规定崩溃点恢复不会返回半发布结果或重复发布；不确定结果显式进入 `OUTCOME_UNKNOWN`。
7. macOS/Linux Bazel Build/Test 与 ASan 稳定通过。
8. 路径逃逸、Secret 外发、越权、Prompt Injection 和资源耗尽安全测试通过。
9. 百万 Chunk 容量与性能基线在第 17 节指定的版本化 Linux 发布基准环境达到全部硬目标；macOS 记录独立单线程基线。任何偏差都阻塞 1.0，除非先通过 ADR 修改目标并重新完成容量评审。
10. 至少 200 条 Retrieval Eval 达到 Phase 2 的质量阈值；至少 50 个 Loom 任务完成重复 A/B，Hybrid 的 Pass@1 不低于 Sparse 基线，安全违规为 0。
11. 备份、恢复、回滚、升级、索引格式迁移和数据删除完成演练。
12. 日志、Trace、缓存、评测产物和备份不含已知秘密或默认正文。

## 22. 关键决策

| 决策 | 选择 | 原因 |
|---|---|---|
| 名称 | Shuttle | 与 Loom 形成“梭子为织布机传递材料”的产品关系 |
| 服务职责 | Evidence Retrieval | 保持 Agent 推理、安全和最终选择在 Loom |
| 核心语言 | C++20 | FAISS/Clang 生态、性能、资源控制和仓库现有基础 |
| 服务协议 | gRPC + Protobuf | Go/C++ 强类型、deadline/cancel、版本演进和流量约束 |
| 首期部署 | 单机本地优先 | 满足个人知识库，避免过早引入分布式复杂度 |
| 元数据 | SQLite WAL | 本地事务、迁移和运维简单 |
| 大对象 | SHA-256 内容寻址文件 | 去重、校验和原子发布，避免数据库膨胀 |
| 索引更新 | Immutable Segment + Manifest | 查询一致、可恢复、可回滚、支持版本共存 |
| 检索 | Sparse + Dense + RRF | 兼顾代码精确标识符与自然语言语义 |
| 向量引擎 | FAISS | 当前仓库已有依赖，C++ 集成成熟 |
| 模型 | 外部 Provider 接口 | 不绑定模型和推理框架，可本地或远程部署 |
| 当前工作树 | 本地工具优先 | 避免索引新鲜度冒充实时文件事实 |
| Memory | 独立服务 | 记忆生命周期与客观知识检索语义不同 |

## 23. 开放问题

以下问题在对应 Phase 前通过 ADR 固化：

1. C++/Go Parser 采用 Tree-sitter、Clang/Go 原生解析器或组合方案，以及依赖的 Bazel/许可证维护方式。
2. Proto 是否继续放共享 `proto/shuttle/v1`，以及 Go Stub 的生成与依赖边界。
3. 默认本地 Embedding 和 Reranker 模型及其硬件兼容矩阵。
4. Query 是否需要 Server Streaming；首版优先 Unary，只有大结果或渐进召回有明确收益时再增加。
5. Git 历史保留深度、Diff 体积和重复内容策略。
6. Worktree Overlay 是由 Shuttle 维护，还是由 Loom 在 Context Manager 层合并。
7. Dense/通用 Segment 元数据格式是自定义、Protobuf framing，还是复用 `sstv2` 的部分通用组件；Sparse 首版已确定为每 Segment 独立只读 FTS5 文件。
8. 远程模型出口审批由 Shuttle 独立执行，还是要求 Loom 签发短期 Capability；默认两侧都校验。
