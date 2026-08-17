# MLX Embedding & Rerank Server

基于 Apple MLX 框架的本地推理服务，专为 Apple Silicon (M1/M2/M3/M4) 优化：

- `POST /v1/embeddings` — OpenAI 兼容的 Embedding API
- `POST /v1/rerank` — Cohere 风格的 Cross-Encoder 重排 API

同时支持 BERT 与 (XLM-)RoBERTa 架构的 checkpoint（自动处理权重前缀、可选
token-type embedding、position id 偏移）；pooling 方式从模型自带的
sentence-transformers `1_Pooling/config.json` 读取（BGE 系列为 CLS）。

## 架构

```
Client  ──HTTP POST──▶  FastAPI (/v1/embeddings, /v1/rerank)
                              │
                ┌─────────────┴─────────────┐
                │   MLXTransformerEncoder   │  共享 BERT/XLM-R 前向
                └─────────────┬─────────────┘
              ┌───────────────┴───────────────┐
        MLXEmbeddingModel               MLXRerankModel
        pooling + L2 norm               [CLS] → classifier → sigmoid
```

## 构建与运行

### 使用 Bazel

```bash
# 构建
bazel build //cpp/pl/minisearch/embedding_server:embedding_server

# 运行（默认加载 BAAI/bge-m3 模型）
bazel run //cpp/pl/minisearch/embedding_server:embedding_server

# 同时加载 rerank 模型（推荐：全本地 hybrid 检索 + 重排）
bazel run //cpp/pl/minisearch/embedding_server:embedding_server -- \
    --model BAAI/bge-m3 \
    --rerank-model BAAI/bge-reranker-v2-m3 \
    --host 0.0.0.0 \
    --port 8000 \
    --max-length 512
```

### 直接运行

```bash
pip install mlx numpy transformers fastapi 'uvicorn[standard]' huggingface-hub safetensors
python cpp/pl/minisearch/embedding_server/server.py \
    --model BAAI/bge-m3 --rerank-model BAAI/bge-reranker-v2-m3
```

## API

### POST /v1/embeddings

OpenAI 兼容的 Embedding 接口。

**请求：**

```json
{
    "input": ["你好世界", "Hello world"],
    "model": "bge-m3"
}
```

也支持单条文本：

```json
{
    "input": "你好世界",
    "model": "bge-m3"
}
```

**响应：**

```json
{
    "object": "list",
    "data": [
        {
            "object": "embedding",
            "embedding": [0.0123, -0.0456, ...],
            "index": 0
        },
        {
            "object": "embedding",
            "embedding": [0.0789, -0.0012, ...],
            "index": 1
        }
    ],
    "model": "bge-m3",
    "usage": {
        "prompt_tokens": 12,
        "total_tokens": 12
    }
}
```

### POST /v1/rerank

Cohere 风格的重排接口（需以 `--rerank-model` 启动）。

**请求：**

```json
{
    "model": "bge-reranker-v2-m3",
    "query": "presto join 阶段 CPU 热点怎么排查",
    "documents": ["当 CPU 热点出现在 join 阶段时……", "会话事件写入 SQLite……"],
    "top_n": 2
}
```

**响应：**

```json
{
    "results": [
        {"index": 0, "relevance_score": 0.93},
        {"index": 1, "relevance_score": 0.04}
    ],
    "model": "bge-reranker-v2-m3"
}
```

### GET /health

健康检查端点（返回已加载的 embedding / rerank 模型名）。

```bash
curl http://localhost:8000/health
```

## 与 MiniSearch Server 集成

在 minisearch_server 启动时指定 embedding / rerank 服务地址：

```bash
./minisearch_server \
    --embedding_endpoint=http://localhost:8000 \
    --embedding_model=bge-m3 \
    --rerank_endpoint=http://localhost:8000 \
    --rerank_model=bge-reranker-v2-m3
```

## 支持的模型

支持 BERT 与 (XLM-)RoBERTa 架构的 HuggingFace 模型，推荐（本地可运行的最佳组合）：

| 用途 | 模型 | 维度/参数量 | 语言 | 说明 |
|------|------|------|------|
| Embedding | BAAI/bge-m3 | 1024 / 568M | 多语言 | 默认推荐，中英文效果优秀 |
| Embedding | BAAI/bge-large-zh-v1.5 | 1024 / 326M | 中文 | 中文专用 |
| Embedding | BAAI/bge-small-zh-v1.5 | 512 / 24M | 中文 | 轻量级 |
| Rerank | BAAI/bge-reranker-v2-m3 | 568M | 多语言 | 与 bge-m3 同家族的最强重排 |
| Rerank | BAAI/bge-reranker-large | 560M | 中英 | 大模型重排 |

## 依赖管理

依赖声明在 `requirements.in`，锁定文件通过以下命令生成：

```bash
uv pip compile cpp/pl/minisearch/embedding_server/requirements.in \
    --python-platform aarch64-apple-darwin \
    --python-version 3.13 \
    -o cpp/pl/minisearch/embedding_server/requirements_lock.txt
```

注意：由于 MLX 仅支持 Apple Silicon，lock 文件限定了 `aarch64-apple-darwin` 平台，
不会影响 Linux CI 上其他 Python target 的构建。
