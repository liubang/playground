# Vector Recall Service Design Document

> Based on FAISS + brpc (HTTP/JSON) + Embedding + MLX Inference | Module: `cpp/pl/recall`

| Field   | Value                          |
| ------- | ------------------------------ |
| Author  | liubang (it.liubang@gmail.com) |
| Version | 4.0                            |
| Status  | Draft                          |

---

## 1. Background & Motivation

在自然语言查询场景中，用户用自然语言描述数据需求，系统需要从成百上千张候选库表中定位到用户真正需要的那几张。整体 pipeline 如下：

```
Natural Language → Embedding → Vector Recall → LLM Table Selection
    → User Confirmation → SQL Generation → Quality Scoring → Submit
```

本文档聚焦 **Vector Recall** 阶段——一个基于 FAISS（Facebook AI Similarity Search）和 brpc 构建的独立 HTTP/JSON 召回服务。它的职责是接收 query embedding（或原始文本），从索引中检索出 top-k 最相似的库表，交给下游 LLM 做精细筛选。

选择 HTTP/JSON 而非 Protobuf RPC 协议，是因为客户端可能是 JavaScript、Java、Python 等多种语言，HTTP/JSON 具有最广泛的兼容性，无需为每种语言生成 protobuf stub。

v3.0 新增了 **Embedding 集成层**：服务端可选地对接 OpenAI 兼容的 Embedding API，客户端可以直接传入原始文本而无需预计算向量，大幅降低了接入门槛。

v4.0 新增了 **MLX Embedding Server**：一个基于 Apple MLX 框架的本地推理服务，专为 Apple Silicon 优化。它提供 OpenAI 兼容的 `/v1/embeddings` 接口，可以直接作为 recall_server 的 embedding 后端，实现完全本地化的端到端向量召回，无需依赖任何外部 API 服务。

## 2. Architecture Overview

召回服务是一个单进程 brpc server，在内存中持有 FAISS 索引。上游服务（agent 编排器、前端应用等）通过 HTTP/JSON API 调用它来写入库表向量和执行近邻检索。服务支持通过快照机制将索引持久化到磁盘，并可选地集成 Embedding 服务实现文本直通。

### 2.1 System Context

```
                                ┌──────────────────────────────────────┐
                                │       Vector Recall Service          │
                                │           (brpc HTTP/JSON)           │
┌─────────────┐  text or vec    │  ┌──────────────────────────────┐   │     top-k tables     ┌───────────────┐
│  Clients     │ ─────────────> │  │  RecallHttpServiceImpl       │   │ ──────────────────> │  LLM Table     │
│  JS / Java / │                │  │  9 HTTP endpoints            │   │                     │  Selector      │
│  Python /    │                │  └──────┬──────────┬────────────┘   │                      └───────────────┘
│  curl        │                │         │          │                │
└─────────────┘                 │    ┌────┴────┐ ┌───┴────────────┐  │
                                │    │  FAISS  │ │ EmbeddingClient│  │
                                │    │  Index  │ │  (optional)    │  │
                                │    └─────────┘ └───────┬────────┘  │
                                └────────────────────────┼───────────┘
                                                         │ HTTP POST
                                                         │ /v1/embeddings
                                                         ▼
                                          ┌──────────────────────────┐
                                          │    Embedding Service      │
                                          │  ┌────────────────────┐  │
                                          │  │ MLX Embedding Server│  │  ◄── 本项目自带
                                          │  │ (Apple Silicon本地) │  │      embedding_server/
                                          │  └────────────────────┘  │
                                          │  or OpenAI / vLLM /      │
                                          │     Ollama / TEI         │
                                          └──────────────────────────┘
```

服务提供两种使用模式：

**向量模式（Vector Mode）**：客户端预计算好 embedding 向量，通过 `/api/recall/add`、`/batch_add`、`/search` 接口直接传入浮点数组。适用于客户端已有 embedding 能力或离线批量灌入的场景。

**文本模式（Text Mode）**：客户端传入原始文本，服务端自动调用 Embedding API 计算向量，再执行索引操作。通过 `/api/recall/add_by_text`、`/batch_add_by_text`、`/search_by_text` 接口使用。适用于客户端不具备 embedding 能力或希望简化调用链路的场景。文本模式需要在启动时配置 Embedding 服务，否则相关接口返回 503。

### 2.2 Internal Components

| Component                 | Responsibility                                                                              |
| ------------------------- | ------------------------------------------------------------------------------------------- |
| **FaissIndex**            | 线程安全的 `faiss::IndexIDMap` 封装，负责 add、search、save、load，所有操作通过 mutex 保护  |
| **IdMapper**              | 字符串 `table_id`（如 `"default.user_info"`）与 FAISS 所需的 `int64` 数值 ID 之间的双向映射 |
| **MetaStore**             | 内存中的 `TableMeta` protobuf 缓存，按 `table_id` 索引，在 Search 结果中附带返回            |
| **RecallHttpServiceImpl** | brpc HTTP 服务实现，通过 `default_method` 接收所有请求，按 URL path 分发到 9 个 handler     |
| **EmbeddingClient**       | Embedding 抽象接口，定义 `Embed(text)` 和 `EmbedBatch(texts)` 方法                          |
| **OpenAIEmbeddingClient** | `EmbeddingClient` 的 OpenAI 兼容实现，通过 brpc HTTP Channel 调用 `/v1/embeddings` 接口     |
| **MLX Embedding Server**  | 基于 Apple MLX 的本地推理服务（Python/FastAPI），提供 OpenAI 兼容 API，可作为上述 Client 的后端 |

## 3. HTTP API Design

服务通过 brpc 的 HTTP 能力对外提供 RESTful JSON API。内部仍使用 protobuf message 作为数据结构（通过 `json2pb` 实现 JSON 与 protobuf 的自动转换），但对外完全是标准的 HTTP/JSON 接口。

### 3.1 brpc HTTP 实现原理

brpc 的 HTTP 服务通过一个特殊的 proto service 实现：定义空的 `HttpRequest`/`HttpResponse` message 和一个 `default_method` RPC，brpc 会将所有匹配的 HTTP 请求路由到这个方法。实际的请求/响应数据通过 `brpc::Controller` 的 `request_attachment()` 和 `response_attachment()` 传递。

```protobuf
// proto 定义（仅用于 brpc HTTP 框架，不暴露给客户端）
message HttpRequest {}
message HttpResponse {}

service RecallHttpService {
  rpc default_method(HttpRequest) returns (HttpResponse);
}
```

服务注册时通过 `restful_mappings` 指定 URL 路由：

```cpp
server.AddService(service.get(), brpc::SERVER_DOESNT_OWN_SERVICE,
    "/api/recall/add               => default_method,"
    "/api/recall/batch_add         => default_method,"
    "/api/recall/search            => default_method,"
    "/api/recall/add_by_text       => default_method,"
    "/api/recall/batch_add_by_text => default_method,"
    "/api/recall/search_by_text    => default_method,"
    "/api/recall/snapshot/save     => default_method,"
    "/api/recall/snapshot/load     => default_method,"
    "/api/recall/stats             => default_method");
```

### 3.2 API Endpoints

服务共提供 9 个 HTTP 端点，分为三组：

**向量接口（Vector API）**——客户端直接传入 embedding 向量：

| Method | Path                    | Description              |
| ------ | ----------------------- | ------------------------ |
| POST   | `/api/recall/add`       | 添加单条库表向量及元信息 |
| POST   | `/api/recall/batch_add` | 批量添加                 |
| POST   | `/api/recall/search`    | 检索 top-k 最近邻库表    |

**文本接口（Text API）**——服务端自动调用 Embedding 服务计算向量：

| Method | Path                            | Description                                |
| ------ | ------------------------------- | ------------------------------------------ |
| POST   | `/api/recall/add_by_text`       | 通过文本添加单条库表（自动计算 embedding） |
| POST   | `/api/recall/batch_add_by_text` | 批量文本添加（一次 batch embedding 调用）  |
| POST   | `/api/recall/search_by_text`    | 通过文本检索（自动计算 query embedding）   |

**管理接口（Admin API）**：

| Method | Path                        | Description                                    |
| ------ | --------------------------- | ---------------------------------------------- |
| POST   | `/api/recall/snapshot/save` | 持久化索引和 ID 映射到磁盘                     |
| POST   | `/api/recall/snapshot/load` | 从磁盘恢复索引和 ID 映射                       |
| GET    | `/api/recall/stats`         | 查询索引状态（向量数、维度、类型、是否已训练） |

### 3.3 Request/Response Examples

**添加单条向量：**

```bash
curl -X POST http://localhost:8200/api/recall/add \
  -H 'Content-Type: application/json' \
  -d '{
    "table_id": "default.user_info",
    "embedding": [0.1, 0.2, ...],
    "meta": {
      "database": "default",
      "table": "user_info",
      "comment": "用户基本信息表"
    }
  }'

# Response: {"success": true, "message": ""}
```

**向量检索：**

```bash
curl -X POST http://localhost:8200/api/recall/search \
  -H 'Content-Type: application/json' \
  -d '{
    "embedding": [0.1, 0.2, ...],
    "top_k": 5
  }'

# Response:
# {
#   "results": [
#     {
#       "table_id": "default.user_info",
#       "distance": 0.123,
#       "meta": {"database": "default", "table": "user_info", "comment": "..."}
#     },
#     ...
#   ]
# }
```

**通过文本添加（自动 embedding）：**

```bash
curl -X POST http://localhost:8200/api/recall/add_by_text \
  -H 'Content-Type: application/json' \
  -d '{
    "table_id": "default.user_info",
    "text": "default.user_info: 用户基本信息表, user_id bigint, name string, age int",
    "meta": {
      "database": "default",
      "table": "user_info",
      "comment": "用户基本信息表"
    }
  }'

# Response: {"success": true, "message": ""}
```

**批量文本添加：**

```bash
curl -X POST http://localhost:8200/api/recall/batch_add_by_text \
  -H 'Content-Type: application/json' \
  -d '{
    "items": [
      {
        "table_id": "default.user_info",
        "text": "default.user_info: 用户基本信息表",
        "meta": {"database":"default","table":"user_info","comment":"用户基本信息表"}
      },
      {
        "table_id": "default.order_detail",
        "text": "default.order_detail: 订单明细表",
        "meta": {"database":"default","table":"order_detail","comment":"订单明细表"}
      }
    ]
  }'

# Response: {"success_count": 2, "fail_count": 0, "message": ""}
```

**通过文本检索：**

```bash
curl -X POST http://localhost:8200/api/recall/search_by_text \
  -H 'Content-Type: application/json' \
  -d '{
    "text": "查找用户相关的表",
    "top_k": 5
  }'

# Response: (same format as /api/recall/search)
```

**查询索引状态：**

```bash
curl http://localhost:8200/api/recall/stats

# Response:
# {
#   "total_vectors": 1000,
#   "dimension": 768,
#   "index_type": "Flat",
#   "is_trained": true
# }
```

**保存/加载快照：**

```bash
curl -X POST http://localhost:8200/api/recall/snapshot/save \
  -H 'Content-Type: application/json' \
  -d '{"path": "/data/recall_snapshot"}'

# Response: {"success": true, "message": "snapshot saved to /data/recall_snapshot"}
```

### 3.4 Error Handling

所有错误以 HTTP 状态码 + JSON body 返回：

| Status Code | Meaning                                                         |
| ----------- | --------------------------------------------------------------- |
| 200         | 成功                                                            |
| 400         | 请求参数错误（JSON 解析失败、维度不匹配）                       |
| 404         | 未知的 API 路径                                                 |
| 500         | 服务内部错误（索引操作失败、embedding 维度不匹配）              |
| 502         | Embedding 服务调用失败（上游返回错误）                          |
| 503         | Embedding 服务未配置（调用 `*_by_text` 接口但未启用 embedding） |

错误响应格式：`{"error": "error description"}`

## 4. Core Design Details

### 4.1 FaissIndex

`FaissIndex` 封装 `faiss::IndexIDMap`，后者在任意 base index 之上添加自定义 int64 ID 支持。base index 通过 `faiss::index_factory` 创建，允许通过字符串描述符灵活选择索引类型（如 `"Flat"`、`"IVF256,Flat"`、`"HNSW32"`）。`IndexIDMap` 通过 `own_fields = true` 接管 base index 的生命周期。

线程安全通过单个 `std::mutex` 实现，保护所有索引操作。虽然这会序列化并发读写，但对于预期的工作负载（写入不频繁、读取为主）已经足够。后续可升级为 `std::shared_mutex` 以支持并发读。

持久化使用 `faiss::write_index` / `faiss::read_index`，将完整索引状态序列化为单个二进制文件。加载时通过 `dynamic_cast` 验证反序列化结果确实是 `IndexIDMap`。

### 4.2 IdMapper

FAISS 要求 int64 ID，但库表标识是字符串。`IdMapper` 维护两个 hash map（`table_to_id_` 和 `id_to_table_`）以及一个单调递增计数器（`next_id_`）。`get_or_assign` 方法是幂等的：对同一个 `table_id` 多次调用始终返回相同的数值 ID。

序列化采用紧凑的二进制格式：

```
[next_id: int64][count: int64][entries...]
  entry = [id: int64][len: int32][table_id_bytes: char[]]
```

格式简单、解析快速、无外部依赖。

### 4.3 MetaStore

`MetaStore` 是一个线程安全的内存 map，从 `table_id` 映射到 `TableMeta` protobuf。在 Add/BatchAdd 时写入，在 Search 时查询以丰富返回结果。当前不持久化到磁盘，重启后需通过 Add API 重新填充。

### 4.4 Service Implementation

`RecallHttpServiceImpl` 继承自 proto 生成的 `RecallHttpService`，实现 `default_method` 作为所有 HTTP 请求的入口。在 `default_method` 中根据 `cntl->http_request().uri().path()` 分发到 9 个 Handle 方法。

JSON 与 protobuf 的转换通过 brpc 内置的 `json2pb` 库实现：`ParseJsonBody<T>` 模板方法从 request body 解析 JSON 到 protobuf message，`SendJsonResponse<T>` 模板方法将 protobuf message 序列化为 JSON 写入 response body。这种设计让内部逻辑仍然享受 protobuf 的类型安全和代码生成便利，同时对外提供标准 JSON 接口。

`Init` 方法提供了一种干净的启动时加载快照的方式，在 server 开始接受连接之前调用，避免构造临时 HTTP 请求。

构造函数接受一个可选的 `shared_ptr<EmbeddingClient>`。当 `embedding_client_` 为 nullptr 时，三个 `*_by_text` handler 会直接返回 503 状态码和 `"embedding service not configured"` 错误信息，实现优雅降级。

## 5. Embedding Integration

### 5.1 Design Goals

Embedding 集成层的设计目标是让客户端可以直接传入原始文本（如库表 schema 描述或用户自然语言查询），由服务端负责调用 Embedding 模型将文本转换为向量，再执行索引操作。这样做的好处是：客户端无需集成 embedding SDK 或管理模型调用逻辑，降低了多语言客户端的接入成本；embedding 模型的选择和配置集中在服务端管理，便于统一切换和优化。

### 5.2 EmbeddingClient Abstraction

`EmbeddingClient` 是一个纯虚基类，定义了两个核心方法：

`Embed(text)` 将单条文本编码为 `vector<float>`，返回 `EmbeddingResult` 结构体（包含 `ok` 标志、`error` 信息和 `embedding` 向量）。

`EmbedBatch(texts)` 将多条文本批量编码，返回 `BatchEmbeddingResult`（包含 `ok` 标志、`error` 信息和 `embeddings` 二维向量）。基类提供了默认实现——逐条调用 `Embed`，子类可以覆盖为真正的 batch API 调用以提升效率。

这个抽象层使得服务可以对接不同的 embedding 后端（OpenAI、本地 sentence-transformers、vLLM、Ollama、HuggingFace TEI 等），只需实现对应的子类。

### 5.3 OpenAIEmbeddingClient

`OpenAIEmbeddingClient` 是当前唯一的具体实现，兼容所有实现了 OpenAI `/v1/embeddings` 接口的服务。它通过 `Options` 结构体配置：

```cpp
struct Options {
    std::string endpoint;      // 如 "http://localhost:11434" 或 "https://api.openai.com"
    std::string path;          // 默认 "/v1/embeddings"
    std::string model;         // 如 "text-embedding-3-small"、"bge-m3"
    std::string api_key;       // 可选，OpenAI 需要，本地服务通常不需要
    int timeout_ms = 30000;    // 超时
    int max_retry = 2;         // 最大重试次数
};
```

内部使用 brpc 的 HTTP Channel（`brpc::PROTOCOL_HTTP`）发送请求，使用 brpc 内置的 rapidjson（`butil/third_party/rapidjson/`，命名空间 `BUTIL_RAPIDJSON_NAMESPACE`）构建和解析 JSON。采用 pimpl 模式隐藏实现细节。

请求格式遵循 OpenAI 标准：单条请求发送 `{"model": "...", "input": "text"}`，批量请求发送 `{"model": "...", "input": ["text1", "text2", ...]}`。响应解析支持 `{"data": [{"embedding": [...], "index": 0}, ...]}` 格式，并正确处理 `index` 字段以应对乱序返回。

`EmbedBatch` 覆盖了基类的默认实现，将所有文本合并为一个 batch 请求发送，避免了逐条调用的网络开销。

### 5.4 Text-based API Handlers

三个文本接口的处理流程如下：

**HandleAddByText**：解析 `AddByTextRequest`（包含 `table_id`、`text`、`meta`），调用 `embedding_client_->Embed(text)` 获取向量，校验维度后写入 FAISS 索引和 MetaStore。

**HandleBatchAddByText**：解析 `BatchAddByTextRequest`，收集所有 item 的 `text` 字段，调用 `embedding_client_->EmbedBatch(texts)` 一次性获取所有向量（避免逐条调用），然后逐条写入索引。

**HandleSearchByText**：解析 `SearchByTextRequest`（包含 `text`、`top_k`），调用 `embedding_client_->Embed(text)` 获取 query 向量，然后执行与 `HandleSearch` 相同的检索逻辑。

所有文本接口在 `embedding_client_` 为 nullptr 时返回 503，在 embedding 调用失败时返回 502。

### 5.5 Graceful Degradation

Embedding 集成是完全可选的。服务启动时，如果未配置 `--embedding_endpoint` 和 `--embedding_model`，则不会创建 `EmbeddingClient`，服务仍然正常运行——6 个向量接口和 3 个管理接口完全可用，只有 3 个 `*_by_text` 接口返回 503。这种设计确保了向后兼容：已有的向量模式客户端不受任何影响。

## 6. MLX Embedding Server

### 6.1 Overview

项目自带了一个基于 Apple MLX 框架的 Embedding 推理服务（`embedding_server/`），专为 Apple Silicon（M1/M2/M3/M4）优化。它提供 OpenAI 兼容的 `/v1/embeddings` HTTP API，可以直接作为 recall_server 的 `--embedding_endpoint` 后端，实现完全本地化的推理链路，无需依赖任何外部 API 服务。

选择 MLX 而非 Ollama 或 llama.cpp 的原因是：MLX 是 Apple 官方为 Apple Silicon 设计的机器学习框架，能够充分利用统一内存架构（UMA）实现 CPU/GPU/ANE 之间的零拷贝数据共享，在 Mac 上的推理性能显著优于通用方案。

### 6.2 Architecture

```
Client  ──HTTP POST──▶  FastAPI (/v1/embeddings)
                              │
                        MLXEmbeddingModel
                              │
                    ┌─────────┴─────────┐
                    │  HF Tokenizer     │  MLX Forward Pass
                    │  (text → tokens)  │  (GPU/ANE inference)
                    └─────────┬─────────┘
                              │
                        Mean Pooling + L2 Norm
                              │
                        JSON Response
```

`MLXEmbeddingModel` 是核心推理类，完整实现了 BERT 架构的 forward pass：加载 HuggingFace safetensors 权重到 MLX array，通过 word/position/token_type embedding → N 层 Transformer encoder（multi-head self-attention + FFN + LayerNorm + residual）→ mean pooling → L2 normalize 得到最终的 embedding 向量。所有矩阵运算都在 MLX 上执行，自动利用 Apple GPU。

FastAPI 应用暴露两个端点：`POST /v1/embeddings`（OpenAI 兼容的 embedding 接口）和 `GET /health`（健康检查）。

### 6.3 Supported Models

理论上支持所有 BERT 架构的 HuggingFace 模型，推荐：

| Model | Dimension | Language | Notes |
| ----- | --------- | -------- | ----- |
| BAAI/bge-m3 | 1024 | 多语言 | 默认推荐，中英文效果优秀 |
| BAAI/bge-large-zh-v1.5 | 1024 | 中文 | 中文专用 |
| BAAI/bge-small-en-v1.5 | 384 | 英文 | 轻量级英文模型 |

首次运行时会自动从 HuggingFace Hub 下载模型权重（BGE-M3 约 2.2GB），之后缓存在本地。

### 6.4 Bazel Integration

Embedding server 使用独立的 pip hub（`pip_embedding`）管理 Python 依赖，与项目主 pip hub 隔离。这是因为 MLX 及其 Metal 后端（`mlx-metal`）仅支持 Apple Silicon，如果混入主 hub 会导致 Linux CI 解析依赖失败。

`MODULE.bazel` 中的配置：

```python
pip.parse(
    experimental_index_url = "https://mirrors.aliyun.com/pypi/simple",
    hub_name = "pip_embedding",
    python_version = "3.13",
    requirements_lock = "//cpp/pl/recall/embedding_server:requirements_lock.txt",
)
use_repo(pip, "pip_embedding")
```

BUILD 文件通过 `@pip_embedding//:requirements.bzl` 的 `requirement()` 宏引用依赖，构建为 `py_binary` target。

依赖锁定文件通过以下命令生成（限定 Apple Silicon 平台）：

```bash
uv pip compile cpp/pl/recall/embedding_server/requirements.in \
    --python-platform aarch64-apple-darwin \
    --python-version 3.13 \
    -o cpp/pl/recall/embedding_server/requirements_lock.txt
```

### 6.5 Usage

```bash
# 构建并运行（默认加载 BAAI/bge-m3）
bazel run //cpp/pl/recall/embedding_server:embedding_server

# 自定义参数
bazel run //cpp/pl/recall/embedding_server:embedding_server -- \
    --model BAAI/bge-m3 --host 0.0.0.0 --port 8000 --max-length 512
```

启动后可以直接测试：

```bash
curl -X POST http://localhost:8000/v1/embeddings \
  -H 'Content-Type: application/json' \
  -d '{"input": ["你好世界", "Hello world"], "model": "bge-m3"}'
```

### 6.6 Integration with Recall Server

将 MLX Embedding Server 作为 recall_server 的 embedding 后端，只需在启动 recall_server 时指向它：

```bash
# 1. 启动 embedding server
bazel run //cpp/pl/recall/embedding_server:embedding_server -- --port 8000

# 2. 启动 recall server，指向 embedding server
bazel run //cpp/pl/recall:recall_server -- \
    --dimension=1024 --port=8200 \
    --embedding_endpoint=http://localhost:8000 \
    --embedding_model=bge-m3
```

此时 recall_server 的所有 `*_by_text` 接口即可正常使用，形成完全本地化的端到端链路：客户端文本 → recall_server → MLX embedding server → FAISS 检索 → 返回 top-k 结果。

## 7. FAISS Integration with Bazel

FAISS v1.14.1 通过 `MODULE.bazel` 中的 `archive_override` 集成到 Bazel 8 构建系统。由于 FAISS 原生不支持 Bazel，通过 patch 文件（`bazel/faiss-bazel8-compat.patch`）添加必要的 `BUILD.bazel` 和 `MODULE.bazel`。

集成过程中解决了以下关键问题：

- **OpenMP 兼容性**：Apple Clang 不支持 `-fopenmp`。创建了 OpenMP stub 头文件（`bazel/omp_stub/omp.h`），为 FAISS 使用的所有 `omp_*` 函数（含 `omp_get_nested`/`omp_set_nested`）提供 no-op 实现。
- **BLAS/LAPACK 链接**：通过 `select()` 规则实现跨平台链接——macOS 使用 Accelerate framework，Linux 使用 OpenBLAS。
- **SIMD dispatch**：为 AVX2、AVX512、NEON、SVE 分别创建 `cc_library` target，配置对应的编译器标志。
- **Target 命名冲突**：`faiss` 目录名与 `faiss` target 名冲突，通过根级 `BUILD.bazel` alias 解决。

## 8. File Structure

```
cpp/pl/recall/
├── proto/
│   ├── recall.proto              # Protobuf messages + HTTP service definition
│   └── BUILD                     # proto_library + cc_proto_library
├── faiss_index.h                 # FaissIndex + IdMapper + RecallResult declarations
├── faiss_index.cpp               # FaissIndex + IdMapper implementations
├── embedding_client.h            # EmbeddingClient interface + OpenAIEmbeddingClient
├── embedding_client.cpp          # OpenAI-compatible embedding implementation (brpc HTTP + rapidjson)
├── recall_service.h              # MetaStore + RecallHttpServiceImpl declarations
├── recall_service.cpp            # HTTP/JSON service implementation (9 handlers, json2pb)
├── recall_server.cpp             # Server main (gflags, embedding init, restful_mappings)
├── recall_client.cpp             # HTTP client example (brpc Channel + HTTP protocol)
├── test.sh                       # End-to-end test script (19 test cases)
├── DESIGN.md                     # This document
├── BUILD                         # Bazel build rules (5 C++ targets)
└── embedding_server/             # MLX-based local embedding inference service
    ├── server.py                 # FastAPI app + MLX BERT forward pass
    ├── requirements.in           # Python dependency declarations
    ├── requirements_lock.txt     # Locked deps (aarch64-apple-darwin only)
    ├── BUILD                     # Bazel py_binary target (pip_embedding hub)
    └── README.md                 # Embedding server usage guide
```

Bazel BUILD 定义了 5 个 C++ target：`faiss_index`（FAISS 封装）、`embedding_client`（embedding 抽象 + OpenAI 实现）、`recall_service_impl`（HTTP 服务，依赖前两者）、`recall_server`（server binary）、`recall_client`（client binary，无 proto 依赖）。`embedding_server/BUILD` 额外定义了 1 个 Python target：`embedding_server`（MLX 推理服务）。

## 9. Build & Run

### 9.1 Build

```bash
# C++ recall service
bazel build //cpp/pl/recall:recall_server //cpp/pl/recall:recall_client

# MLX embedding server (Apple Silicon only)
bazel build //cpp/pl/recall/embedding_server:embedding_server
```

### 9.2 Server Startup

Server 支持以下 gflags 参数：

| Flag                     | Default          | Description                                       |
| ------------------------ | ---------------- | ------------------------------------------------- |
| `--port`                 | `8200`           | 监听端口                                          |
| `--idle_timeout_s`       | `-1`             | 连接空闲超时（-1 = 禁用）                         |
| `--dimension`            | `768`            | Embedding 向量维度                                |
| `--index_type`           | `Flat`           | FAISS 索引描述字符串                              |
| `--snapshot_path`        | (empty)          | 启动时加载快照的路径                              |
| `--embedding_endpoint`   | (empty)          | Embedding 服务地址（如 `http://localhost:11434`） |
| `--embedding_model`      | (empty)          | Embedding 模型名称（如 `bge-m3`）                 |
| `--embedding_api_key`    | (empty)          | Embedding API key（可选）                         |
| `--embedding_path`       | `/v1/embeddings` | Embedding API 路径                                |
| `--embedding_timeout_ms` | `30000`          | Embedding 请求超时（毫秒）                        |

启动示例（仅向量模式）：

```bash
bazel run //cpp/pl/recall:recall_server -- \
    --dimension=768 --index_type=Flat --port=8200
```

启动示例（启用 Embedding，对接本地 MLX Embedding Server）：

```bash
# 先启动 embedding server（另一个终端）
bazel run //cpp/pl/recall/embedding_server:embedding_server -- --port 8000

# 再启动 recall server
bazel run //cpp/pl/recall:recall_server -- \
    --dimension=1024 --port=8200 \
    --embedding_endpoint=http://localhost:8000 \
    --embedding_model=bge-m3
```

启动示例（启用 Embedding，对接 Ollama）：

```bash
bazel run //cpp/pl/recall:recall_server -- \
    --dimension=1024 --port=8200 \
    --embedding_endpoint=http://localhost:11434 \
    --embedding_model=bge-m3
```

启动示例（对接 OpenAI）：

```bash
bazel run //cpp/pl/recall:recall_server -- \
    --dimension=1536 --port=8200 \
    --embedding_endpoint=https://api.openai.com \
    --embedding_model=text-embedding-3-small \
    --embedding_api_key=sk-xxx
```

带快照恢复：

```bash
bazel run //cpp/pl/recall:recall_server -- \
    --dimension=768 --snapshot_path=/data/recall_snapshot
```

### 9.3 Client Example

示例客户端使用 brpc 的 HTTP Channel 发送 JSON 请求，演示完整生命周期：逐条添加 5 条 demo 库表向量、批量添加 2 条、查询索引状态、执行 top-3 检索、保存和加载快照。

```bash
bazel run //cpp/pl/recall:recall_client -- \
    --server=http://127.0.0.1:8200 --dimension=768
```

也可以直接用 curl 测试：

```bash
# 添加向量
curl -X POST http://localhost:8200/api/recall/add \
  -H 'Content-Type: application/json' \
  -d '{"table_id":"default.user_info","embedding":[0.1,0.2,...]}'

# 通过文本添加（需要启用 embedding）
curl -X POST http://localhost:8200/api/recall/add_by_text \
  -H 'Content-Type: application/json' \
  -d '{"table_id":"default.user_info","text":"用户基本信息表"}'

# 通过文本检索
curl -X POST http://localhost:8200/api/recall/search_by_text \
  -H 'Content-Type: application/json' \
  -d '{"text":"查找用户相关的表","top_k":5}'

# 查看状态
curl http://localhost:8200/api/recall/stats
```

### 9.4 End-to-End Testing

项目包含一个端到端测试脚本 `test.sh`，覆盖 19 个测试用例：

```bash
bash cpp/pl/recall/test.sh
```

测试流程：编译 server → 启动 server（dim=4, port=18200）→ curl 打全部 API → 关闭 server → 汇总结果。测试覆盖了正常流程（add、batch_add、search、snapshot save/load、stats）、错误处理（维度不匹配 → 400、无效 JSON → 400、未知路径 → 404）以及 embedding 未配置时的降级行为（`*_by_text` → 503）。

## 10. Index Type Selection Guide

`--index_type` 接受任意 FAISS index factory 字符串，选择取决于数据规模和延迟要求：

| Index Type     | Scale      | Search Time | Memory          | Notes                                   |
| -------------- | ---------- | ----------- | --------------- | --------------------------------------- |
| `Flat`         | < 100K     | O(n)        | 4 × n × d bytes | 精确搜索，无需训练。小数据集首选        |
| `IVF256,Flat`  | 100K – 10M | O(n/nprobe) | ~4 × n × d      | 需要训练。通过 nprobe 调节速度/召回率   |
| `HNSW32`       | 100K – 10M | O(log n)    | ~1.5× Flat      | 无需训练。高召回率 + 快速检索，内存较高 |
| `IVF4096,PQ32` | > 10M      | O(n/nprobe) | ~32 × n bytes   | 向量压缩。需要训练。低内存，近似结果    |

对于库表召回场景，通常候选表在数千到数万量级，`Flat` 索引即可满足需求（毫秒级延迟）。

## 11. Future Enhancements

1. **MetaStore 持久化**：将 `TableMeta` 与 FAISS 索引快照一起序列化，避免重启后重新填充。
2. **读写锁**：将 `std::mutex` 替换为 `std::shared_mutex`，允许 Search 并发读。
3. **索引训练 pipeline**：为 IVF/PQ 类索引提供训练 API 或离线训练工具，接收代表性向量样本。
4. **增量更新**：支持向量删除和替换，适应库表 schema 变更。
5. **多索引支持**：在单个服务实例中支持多个命名索引（如按业务域或数据库分组）。
6. **监控**：通过 brpc 内置的 `/vars` endpoint 暴露 Prometheus 指标（QPS、延迟分位数、索引大小、内存使用）。
7. **Embedding 缓存**：对相同文本的 embedding 结果进行缓存，减少重复调用 embedding 服务。
8. **认证鉴权**：添加 API key 或 token 认证机制，保护写入接口。
9. **CORS 支持**：为浏览器端 JS 客户端添加 CORS 头，支持跨域请求。
10. **Embedding Server 动态 batching**：为 MLX Embedding Server 添加请求队列和动态 batching，合并短时间内的多个请求以提升吞吐量。
11. **模型热切换**：支持 Embedding Server 在运行时切换模型，无需重启服务。
