# MLX Embedding Server

基于 Apple MLX 框架的 Embedding 推理服务，提供 OpenAI 兼容的 `/v1/embeddings` HTTP API。
专为 Apple Silicon (M1/M2/M3/M4) 优化，充分利用统一内存架构 (UMA) 实现高性能推理。

## 架构

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

## 构建与运行

### 使用 Bazel

```bash
# 构建
bazel build //cpp/pl/recall/embedding_server:embedding_server

# 运行（默认加载 BAAI/bge-m3 模型）
bazel run //cpp/pl/recall/embedding_server:embedding_server

# 自定义参数
bazel run //cpp/pl/recall/embedding_server:embedding_server -- \
    --model BAAI/bge-m3 \
    --host 0.0.0.0 \
    --port 8000 \
    --max-length 512
```

### 直接运行

```bash
pip install mlx numpy transformers fastapi 'uvicorn[standard]' huggingface-hub safetensors
python cpp/pl/recall/embedding_server/server.py --model BAAI/bge-m3
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

### GET /health

健康检查端点。

```bash
curl http://localhost:8000/health
```

## 与 Recall Service 集成

在 recall_server 启动时指定 embedding 服务地址：

```bash
./recall_server \
    --embedding_url=http://localhost:8000/v1/embeddings \
    --embedding_model=bge-m3 \
    --embedding_dim=1024
```

## 支持的模型

理论上支持所有 BERT 架构的 HuggingFace 模型，推荐：

| 模型 | 维度 | 语言 | 说明 |
|------|------|------|------|
| BAAI/bge-m3 | 1024 | 多语言 | 默认推荐，中英文效果优秀 |
| BAAI/bge-large-zh-v1.5 | 1024 | 中文 | 中文专用 |
| BAAI/bge-small-en-v1.5 | 384 | 英文 | 轻量级英文模型 |

## 依赖管理

依赖声明在 `requirements.in`，锁定文件通过以下命令生成：

```bash
uv pip compile cpp/pl/recall/embedding_server/requirements.in \
    --python-platform aarch64-apple-darwin \
    --python-version 3.13 \
    -o cpp/pl/recall/embedding_server/requirements_lock.txt
```

注意：由于 MLX 仅支持 Apple Silicon，lock 文件限定了 `aarch64-apple-darwin` 平台，
不会影响 Linux CI 上其他 Python target 的构建。
