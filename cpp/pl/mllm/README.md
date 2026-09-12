# mllm — Apple Silicon 本地多模态 LLM 推理引擎

mllm 是一个面向 Apple Silicon 的本地大语言模型推理引擎，使用 C++20（Metal 边界为 Objective-C++）实现，直接从 GGUF 文件加载模型，支持 Metal GPU 与 CPU 两种计算后端。除纯文本生成外，还内置 PaddleOCR-VL 视觉塔（多模态 OCR）与一个 OpenAI 兼容的 HTTP 服务。

设计目标：以一条小而完整的端到端推理路径为核心，以 llama.cpp 作为正确性与性能参照，逐步替换热点 kernel。

- 系统设计：见 [docs/SPEC.md](docs/SPEC.md)
- 性能回归基线：见 [docs/PERF_BASELINE.md](docs/PERF_BASELINE.md)

## 特性

- **模型格式**：GGUF v3，权重通过 mmap 零拷贝加载。
- **双后端**：metal（GPU，decode 优化，推荐实际推理）与 cpu（纯 C++20 参考实现，用于正确性/调试）。
- **KV cache**：strict 与 ring（滑动窗口）两种模式；ring 模式超窗自动丢弃最旧 token，可无限长度生成。
- **采样**：greedy、temperature、top-k、top-p、repeat penalty，seed 可复现。
- **Chat 模板**：ChatML / Llama-2 / Llama-3 / PaddleOCR-VL 家族，取自 GGUF 元数据。
- **多模态**：PaddleOCR-VL 视觉塔（SigLIP-derived NaViT encoder + MLP projector），Qwen2-VL 智能缩放与 bicubic 预处理。
- **HTTP 服务**：OpenAI 兼容 `/v1/chat/completions` 与专用 `/v1/ocr`，单 Engine 常驻进程。
- **验证体系**：逐 kernel Metal/CPU 一致性单测 + 与 llama.cpp 的 greedy parity + 真实模型 golden OCR 回归。

## 环境要求

- macOS 14+，Apple Silicon（arm64）。Metal 后端仅 macOS 可用；CPU 后端可跨平台编译。
- Xcode Command Line Tools 提供的 Apple Clang。
- Bazel（版本由仓库根 `.bazelversion` 锁定，直接用 `bazel` 命令即可）。

## 构建

在**仓库根目录**执行：

```bash
# CLI（推理命令行）
bazel build //cpp/pl/mllm/cli:mllm_cli --config=release

# HTTP 服务（OpenAI 兼容）
bazel build //cpp/pl/mllm/server:mllm_server --config=release

# 整个项目（含全部单元测试目标）
bazel build //cpp/pl/mllm/...
```

产物位于 `bazel-bin/cpp/pl/mllm/...`。

## 拉取模型

### 方式一：直接下载 GGUF（推荐）

Hugging Face 上官方或社区已发布现成 GGUF，下载后可直接使用。以 Qwen3-0.6B 为例：

```bash
huggingface-cli download Qwen/Qwen3-0.6B-GGUF Qwen3-0.6B-Q8_0.gguf --local-dir <models-dir>
```

也可以直接下载社区量化的 Qwen2 / TinyLlama GGUF，如 Qwen/Qwen2.5-0.5B-Instruct-GGUF、TinyLlama/TinyLlama-1.1B-Chat-v1.0。

### 方式二：从 safetensors 自行转换

借助 llama.cpp 的转换脚本把 Hugging Face 原始权重转成 GGUF：

```bash
git clone https://github.com/ggml-org/llama.cpp
pip install -r llama.cpp/requirements.txt
python llama.cpp/convert_hf_to_gguf.py <hf-model-dir> \
    --outfile <models-dir>/<name>-f16.gguf --outtype f16
# 可选：进一步量化
llama-quantize <models-dir>/<name>-f16.gguf <models-dir>/<name>-q8_0.gguf Q8_0
```

转换产物需包含 tokenizer 元数据（`tokenizer.ggml.*`），`convert_hf_to_gguf.py` 默认会写入。

## 文本生成（CLI）

```bash
CLI=bazel-bin/cpp/pl/mllm/cli/mllm_cli

# 续写模式（raw prompt），Metal 后端，greedy（temperature 默认 0）
$CLI -m <models-dir>/Qwen3-0.6B-Q8_0.gguf -p "The capital of France is" -n 64 \
     --backend metal

# 对话模式：自动应用模型的 chat 模板
$CLI -m <models-dir>/Qwen3-0.6B-Q8_0.gguf -p "讲一个关于机器人的故事" -n 256 \
     --backend metal --chat

# 采样参数
$CLI -m <model.gguf> -p "Hello" -n 128 -t 0.7 --top-k 40 --top-p 0.9 -s 42

# CPU 后端（调试/对照用，速度明显更慢）
$CLI -m <model.gguf> -p "Hello" -n 64 --backend cpu
```

### 滑动窗口（ring）KV cache

默认（strict）模式下 prompt + 生成长度超过上下文窗口会直接报错。开启 ring 模式后，KV cache 变为滑动窗口：序列超出窗口时最旧 token 被分块压缩丢弃，生成可无限继续。

```bash
$CLI -m <model.gguf> -p "Write a very long story." -n 2000 --ring --ctx 512 \
     --backend metal
```

注意：窗口外的上下文对模型不可见，超长剧情可能「忘记」开头，这是滑动窗口的固有语义。

## 多模态 OCR（CLI）

```bash
# --image 会拼接到 prompt 的图像占位符处；prompt 无占位符时自动前置
$CLI -m paddleocr-vl-1.6.gguf --mmproj paddleocr-vl-1.6-mmproj.gguf \
     -p "OCR:" --image doc.png -n 256 --backend metal
```

`--image` 可重复传入多张图；`--mmproj` 为空时 `--image` 会报错。

## HTTP 服务

```bash
bazel-bin/cpp/pl/mllm/server/mllm_server \
  --model <model.gguf> --mmproj <mmproj.gguf> --backend metal --port 8310
```

端点：

- `GET  /healthz` → `{"status":"ok"}`
- `GET  /v1/models` → OpenAI 模型列表（单条）
- `POST /v1/ocr` → 图像 OCR（`image` 为 base64 或 data URL，可选 `prompt` / `max_tokens`）
- `POST /v1/chat/completions` → OpenAI 兼容对话（content 可含 `image_url` data URL；暂不支持流式）

```bash
curl -s localhost:8310/healthz
curl -s localhost:8310/v1/models

# OCR
curl -s localhost:8310/v1/ocr \
  -d '{"image":"<base64-or-data-url>","prompt":"OCR:"}'

# OpenAI 兼容对话
curl -s localhost:8310/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"mllm","messages":[{"role":"user","content":"Hello"}]}'
```

完整参数见 `mllm_cli -h` 与 `mllm_server --help`。

## 支持的模型

### 架构

根据 GGUF 元数据中的 `general.architecture` 字段识别：

| 架构  | 说明                          | 典型模型                        |
| ----- | ----------------------------- | ------------------------------- |
| llama | LLaMA-compatible decoder-only | LLaMA 2/3、TinyLlama 等同构模型 |
| qwen2 | 附加 Q/K/V projection bias    | Qwen2 / Qwen2.5 dense 系列      |
| qwen3 | RoPE 前逐 head Q/K RMSNorm    | Qwen3 dense 系列                |

### 量化与视觉

- 权重量化：F32、F16、Q8_0、Q4_0（block-wise，block size 32）。K-quants 暂不支持。
- 视觉塔：PaddleOCR-VL（GGUF `clip.projector_type=paddleocr`）。

## 文档

- [docs/SPEC.md](docs/SPEC.md) — 系统设计：范围、架构、接口边界、验证策略与交付状态。
- [docs/PERF_BASELINE.md](docs/PERF_BASELINE.md) — 性能与正确性回归基线（含历史记录）。
- [docs/system_architecture.svg](docs/system_architecture.svg) — 系统架构。
- [docs/inference_pipeline.svg](docs/inference_pipeline.svg) — 推理流水线。
- [docs/memory_ownership.svg](docs/memory_ownership.svg) — 内存与所有权模型。

## 目录结构

```
cli/          命令行入口
server/       OpenAI 兼容 HTTP 服务（brpc）
engine/       推理引擎（prefill/decode 调度、多模态拼接、性能统计）
loader/       GGUF 解析 + mmap 零拷贝张量映射
model/        架构注册表、dense decoder、transformer layer
kv_cache/     KV cache（strict / ring / shell 三种形态）
tokenizer/    BPE tokenizer（llama / gpt2 两个族）
sampler/      greedy / temperature / top-k / top-p / repeat penalty
vision/       视觉塔（PaddleOCR-VL：NaViT encoder + MLP projector）
media/        图像表示与预处理（解码由宿主完成，核心库零第三方依赖）
backend/      backend 抽象；metal/（GPU）与 cpu/（参考实现）
core/         Shape / DType / TensorView / Status 等基础类型
ut/           单元测试（testdata 内含 GGUF fixture 生成器）
e2e/          llama.cpp parity + 真实模型 golden OCR（manual）
tools/        tiny model fixture 生成与 logits 导出
bench/        端到端与 kernel 级基准
docs/         设计文档、性能基线、架构示意图
```

## 测试与 benchmark

```bash
# 单元测试（kernel 级 Metal/CPU 一致性 + 引擎级 E2E）
bazel test //cpp/pl/mllm/... --config=release

# decode 微基准
bazel run //cpp/pl/mllm/bench:bench_decode --config=release -- -m <model.gguf> -n 256

# op 微基准
bazel run //cpp/pl/mllm/bench:bench_ops --config=release
```

真实模型端到端测试（manual tag，不进默认 CI）与完整回归方法见 [docs/PERF_BASELINE.md](docs/PERF_BASELINE.md)。
