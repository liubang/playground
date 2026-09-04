# mllm — Apple Silicon 本地 LLM 推理引擎

mllm 是一个面向 Apple Silicon 的本地大语言模型推理引擎，使用 C++20（Metal 边界为 Objective-C++）实现，直接从 GGUF 文件加载模型，支持 Metal GPU 与 CPU 两种计算后端。设计目标是：以一条小而完整的端到端推理路径为核心，以 llama.cpp 作为正确性与性能参照，逐步替换热点 kernel。

详细设计文档见 [SPEC.md](SPEC.md)。

## 特性

- **模型格式**：GGUF v3，权重通过 mmap 零拷贝加载。
- **后端**：
  - metal（推荐用于实际推理）：Metal compute shader + MPS GEMM，K/V cache 常驻显存（device-resident），decode 阶段面向性能优化。
  - cpu：纯 C++20 参考实现，用于正确性校验与调试；也是 CLI 未指定 --backend 时的默认后端。
- **KV cache**：预分配连续窗口；支持滑动窗口（ring）模式，超出窗口的序列自动丢弃最旧 token，可无限长度生成。
- **采样**：greedy、temperature、top-k、top-p、repeat penalty，可固定 seed 复现。
- **Chat 模板**：开启 chat 模式后自动按模型 GGUF 元数据渲染 ChatML / Llama-2 / Llama-3 家族模板。
- **验证体系**：逐 kernel Metal/CPU 一致性单测 + 真实模型与 llama.cpp 的 greedy 输出 parity 端到端测试（空白归一后逐字节比对；缺 llama-cli/模型时自动 SKIP）。

## 支持的模型

### 架构

根据 GGUF 元数据中的 general.architecture 字段识别：

| 架构 | 说明 | 典型模型 |
|------|------|----------|
| llama | LLaMA-compatible decoder-only | LLaMA 2/3、TinyLlama 等同构模型 |
| qwen2 | 附加 Q/K/V projection bias | Qwen2 / Qwen2.5 dense 系列 |
| qwen3 | RoPE 前逐 head Q/K RMSNorm | Qwen3 dense 系列 |

新增架构族只需在 model/architecture.h 注册结构差异（feature flag），MoE/MLA 等结构不同的族可接入独立的 Model 实现。

### 权重量化

F32、F16、Q8_0、Q4_0（Q8_0/Q4_0 为 block-wise 量化，block size 32）。K-quants 暂不支持。

### Tokenizer

GGUF 内嵌 BPE，按 tokenizer.ggml.model 支持两个族：llama（SentencePiece-compatible，byte fallback）与 gpt2（GPT-2 byte-level BPE，Qwen2/Qwen3 使用）。含 BOS/EOS、特殊 token（如 ChatML 分隔符、think 标签）整词匹配、UTF-8 边界处理；token ids 与 llama.cpp 逐一对齐。

## 编译

### 环境要求

- macOS 14+，Apple Silicon（arm64）；Metal 后端仅 macOS 可用，CPU 后端可跨平台。
- Xcode Command Line Tools 提供的 Apple Clang。
- Bazel（版本由仓库根目录 .bazelversion 锁定，直接用 bazel 命令即可）。

### 构建

在**仓库根目录**执行：

```bash
# 构建 CLI（release 优化）
bazel build //cpp/pl/mllm/cli:mllm_cli --config=release

# 构建整个项目
bazel build //cpp/pl/mllm/...
```

产物位于 bazel-bin/cpp/pl/mllm/cli/mllm_cli。

### 测试

```bash
# 全部单元测试（kernel 级 Metal/CPU 一致性 + 引擎级 E2E）
bazel test //cpp/pl/mllm/...

# 真实模型端到端 parity 测试（vs llama.cpp；manual，不进默认 CI）
bazel test //cpp/pl/mllm/e2e:e2e_llama_parity --config=release --test_output=all
# 前置：llama-cli 在 PATH 中，模型放在 /tmp/mllm_models 下（脚本见 e2e/parity_vs_llamacpp.sh；前置缺失自动 SKIP）
```

### Benchmark

```bash
bazel run //cpp/pl/mllm/bench:bench_decode --config=release -- -m <model.gguf> -n 256
bazel run //cpp/pl/mllm/bench:bench_ops --config=release
```

## 拉取模型

### 方式一：直接下载 GGUF（推荐）

Hugging Face 上官方或社区已发布现成 GGUF，下载后可直接使用。以 Qwen3-0.6B 为例：

```bash
# 使用 huggingface-cli（pip install -U "huggingface_hub[cli]"）
huggingface-cli download Qwen/Qwen3-0.6B-GGUF Qwen3-0.6B-Q8_0.gguf --local-dir <models-dir>

# 或用 llama.cpp 的 hf 下载能力
llama-cli -hf Qwen/Qwen3-0.6B-GGUF:Q8_0   # 会下载到其模型缓存目录

# e2e parity 测试要求模型统一放在 /tmp/mllm_models 下
```

也可以直接下载社区量化的 Qwen2 / TinyLlama GGUF，如 Qwen/Qwen2.5-0.5B-Instruct-GGUF、TinyLlama/TinyLlama-1.1B-Chat-v1.0 的社区 GGUF 版本。

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

转换产物需包含 tokenizer 元数据（tokenizer.ggml.*），convert_hf_to_gguf.py 默认会写入。

## 执行推理

```bash
CLI=bazel-bin/cpp/pl/mllm/cli/mllm_cli

# 续写模式（raw prompt），Metal 后端，greedy（temperature 默认 0）
$CLI -m <models-dir>/Qwen3-0.6B-Q8_0.gguf -p "The capital of France is" -n 64 \
     --backend metal

# 对话模式：自动应用模型的 chat 模板，等价于 llama.cpp 的对话格式
$CLI -m <models-dir>/Qwen3-0.6B-Q8_0.gguf -p "讲一个关于机器人的故事" -n 256 \
     --backend metal --chat

# 采样参数
$CLI -m <model.gguf> -p "Hello" -n 128 -t 0.7 --top-k 40 --top-p 0.9 -s 42

# CPU 后端（调试/对照用，速度明显更慢）
$CLI -m <model.gguf> -p "Hello" -n 64 --backend cpu
```

### 滑动窗口（ring）KV cache

默认（strict）模式下，prompt + 生成长度超过上下文窗口会直接报错。开启 ring 模式后，KV cache 变为滑动窗口：序列超出窗口时最旧的 token 被分块压缩丢弃（约 ceil(窗口/2) 一次，摊销 O(1)），生成可无限继续；RoPE 保持绝对位置编码，attention 只作用于保留窗口。

```bash
# 窗口 512，生成长度不受限
$CLI -m <model.gguf> -p "Write a very long story." -n 2000 --ring --ctx 512 \
     --backend metal
```

注意：窗口外的上下文对模型不可见，超长剧情可能"忘记"开头，这是滑动窗口的固有语义。

### 完整参数

```
usage: mllm_cli -m <model.gguf> -p <prompt> [-n max_tokens] [-t temp]
       [-s seed] [--backend cpu|metal] [--ring] [--ctx <n>] [--chat]
       [--system <msg>] [--repeat-penalty <f>] [--top-k <k>] [--top-p <p>]

  -m                GGUF 模型文件路径（必填）
  -p                prompt 文本（必填）
  -n                最大生成 token 数（默认 128）
  -t                temperature（默认 0，即 greedy）
  -s                采样随机种子（默认 0）
  --backend         cpu | metal（默认 cpu）
  --ring            开启滑动窗口 KV cache
  --ctx n           最大上下文 / KV cache 容量（默认 4096）；ring 模式下即窗口大小
  --chat            按模型元数据渲染 chat 模板
  --system          system prompt（隐含开启 --chat）
  --repeat-penalty  重复惩罚系数
  --top-k           top-k 采样（0 关闭）
  --top-p           top-p 采样
```

生成文本输出到 stdout，性能统计（prefill/decode 耗时、tok/s、TTFT）输出到 stderr。

## 目录结构

```
cli/         命令行入口
engine/      推理引擎（prefill/decode 调度、滑窗管理、性能统计）
loader/      GGUF 解析 + mmap 零拷贝张量映射
model/       架构注册表、dense decoder、transformer layer
kv_cache/    KV cache（strict / ring 两种模式）
tokenizer/   BPE tokenizer（llama / gpt2 两个族）
sampler/     greedy / temperature / top-k / top-p / repeat penalty
backend/     后端抽象；metal/（GPU）与 cpu/（参考实现）
core/        Shape / DType / TensorView / Status 等基础类型
ut/          单元测试（testdata 内含 GGUF fixture 生成器）
e2e/         与 llama.cpp 的真实模型 parity 测试（manual）
tools/       小模型 fixture 生成与 logits 导出工具
bench/       端到端与 kernel 级基准
doc/         架构、推理流水线与内存所有权示意图
SPEC.md      系统设计文档（范围、接口边界、验收标准）
```
