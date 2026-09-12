# mllm 性能与正确性回归基线

每次性能相关改动后重新跑本文件的回归并更新对应小节（保留历史小节便于对比）。

## 回归方法

### 0. 准备

```bash
# 测试图像（确定性生成，依赖 Pillow）
python3 cpp/pl/mllm/bench/testdata/gen_doc_images.py /tmp

MODELS=<path-to-models>  # machine-local dir holding the PaddleOCR-VL-1.6 GGUF weights
M=$MODELS/paddleocr-vl-1.6/PaddleOCR-VL-1.6-GGUF.gguf
MM=$MODELS/paddleocr-vl-1.6/PaddleOCR-VL-1.6-GGUF-mmproj.gguf
CLI=./bazel-bin/cpp/pl/mllm/cli/mllm_cli
bazel build //cpp/pl/mllm/cli:mllm_cli //cpp/pl/mllm/bench:bench_decode //cpp/pl/mllm/bench:bench_ops --config=release
```

### 1. 单元测试

```bash
bazel test //cpp/pl/mllm/... --config=release --test_output=errors
```

### 1.5 真实模型 golden 回归（自动化）

`//cpp/pl/mllm/e2e:e2e_golden_ocr`（manual tag）：用真实 LM + mmproj 跑
text_short / text_long / ocr_small / ocr_large 四个用例，断言 (a) stdout
SHA-256 等于冻结的 golden hash，(b) OCR 输出正确转写出测试图像中的已知文字
（绝对答案，而非仅 backend 间相对一致），并打印性能表：

```bash
bazel test //cpp/pl/mllm/e2e:e2e_golden_ocr --config=release --test_output=all \
    --test_env=MLLM_MODELS_DIR=$MODELS
# 或直接：MLLM_MODELS_DIR=$MODELS ./cpp/pl/mllm/e2e/golden_ocr_regression.sh \
#          ./bazel-bin/cpp/pl/mllm/cli/mllm_cli
```

### 2. 正确性（CPU vs Metal 输出对比）

```bash
# 纯文本短 prompt（前 ~24 token 后可能因 f16 精度漂移分叉 —— 属预期，记录匹配前缀长度）
$CLI -m $M -p "OCR Recognition:" -n 32 --backend {cpu,metal} --ctx 8192

# 纯文本长 prompt（~175 tokens，32 生成 token 应字节一致）
#   prompt 见下文「固定 prompt」小节
$CLI -m $M -p "$LONGP" -n 32 --backend {cpu,metal} --ctx 8192

# OCR 小图 / 大图（48 生成 token 应字节一致）
$CLI -m $M --mmproj $MM -p "OCR:" --image /tmp/doc_small.png -n 48 --backend {cpu,metal} --ctx 8192
$CLI -m $M --mmproj $MM -p "OCR:" --image /tmp/doc_large.png -n 48 --backend {cpu,metal} --ctx 8192
```

### 3. 性能

```bash
# decode 微基准
./bazel-bin/cpp/pl/mllm/bench/bench_decode -m $M -n 64 --backend {metal,cpu}
# op 微基准
./bazel-bin/cpp/pl/mllm/bench/bench_ops --backend {metal,cpu}
# 端到端（跑 3 次，shader 预热后取后两次）
$CLI -m $M --mmproj $MM -p "OCR:" --image /tmp/doc_small.png -n 16 --backend metal --ctx 8192
$CLI -m $M --mmproj $MM -p "OCR:" --image /tmp/doc_large.png -n 16 --backend metal --ctx 8192
$CLI -m $M -p "$LONGP" -n 16 --backend {metal,cpu} --ctx 8192
$CLI -m $M --mmproj $MM -p "OCR:" --image /tmp/doc_{small,large}.png -n 16 --backend cpu --ctx 8192
```

### 固定 prompt

```bash
LONGP="OCR: The quick brown fox jumps over the lazy dog. Invoices total one thousand dollars. Page one of three report two thousand twenty six. Packing list shipper consignee notify party vessel voyage bill of lading number container seal weight measurement. Description of goods harmonized code quantity unit price amount. Terms and conditions apply to all shipments. Payment due within thirty days of invoice date. Late payments accrue interest at one percent per month."
```

---

## 基线 2026-09-12（MPS-tiled vision attention + chunk 256 + tanh/KV 修复）

**环境**: Apple M4 Pro, macOS (darwin_arm64), `--config=release`，PaddleOCR-VL-1.6 GGUF (BF16 LM + BF16/F32 mmproj)，base commit `8d624895a` + MPS-tiled attention / kPrefillChunk=256 / GELU tanh clamp / mrope device-KV 修复。

### 单元测试

`//cpp/pl/mllm/...` — **12/12 PASSED**（含新增 `MetalParityTest.AttentionFullParityPaddleOcrDims`，hd=72/n=632 真实 ViT 维度）。

### 真实模型 golden 回归（e2e_golden_ocr）

**6/6 PASSED**（Metal backend，commit `979aa3d32`）：

| 检查 | 结果 |
|---|---|
| text_short / text_long stdout SHA-256 == golden | PASS |
| ocr_small stdout SHA-256 == golden | PASS |
| ocr_small 语义：转写出图中已知文字（"The quick brown fox…"，"$1,024.50"） | PASS |
| ocr_large stdout SHA-256 == golden | PASS |
| ocr_large 心智检查：非空、无 `<unk>` | PASS |

golden hash 冻结于 Metal + Apple M4 Pro + `--config=release`（见
`e2e/golden_ocr_regression.sh` 头部注释），重复运行逐字节稳定。text_long /
ocr_small / ocr_large 在 CPU 上 hash 同样成立（两后端字节一致）；text_short
在 CPU 上降为 WARN（f16 漂移，见「已知良性差异」）。

单次冷进程性能（回归脚本输出，参考值；正式对比用下文 warm 中位数）：

| case | backend | prompt tok | gen tok | prefill ms | decode ms | tok/s |
|---|---|---|---|---|---|---|
| text_short | metal | 5 | 32 | 44.56 | 165.20 | 193.71 |
| text_long | metal | 175 | 32 | 67.40 | 187.54 | 170.63 |
| ocr_small | metal | 197 | 48 | 601.69 | 324.01 | 148.14 |
| ocr_large | metal | 869 | 48 | 2763.63 | 404.89 | 118.55 |

### 正确性

| 用例 | prompt tokens | 生成 | CPU vs Metal |
|---|---|---|---|
| 纯文本短 prompt | ~6 | 32 | 前 24 token 一致后分叉（f16 精度漂移，与改动前 HEAD 行为相同，非回归） |
| 纯文本长 prompt | 175 | 32 | **字节一致** |
| OCR doc_small (768 patches) | 197 | 48 | **字节一致** |
| OCR doc_large (3456 patches) | 869 | 48 | **字节一致** |

注：基线修复了两个此前阻塞 Metal OCR 的 bug（GELU tanh fast-math 溢出 NaN；mrope decode device-KV 用 rope 位置做物理寻址）。修复前 Metal 上 OCR 输出全 `<unk>` 或乱码。

### 性能 — decode 微基准（bench_decode, n=64, EOS 于 8 token）

| backend | tok/s |
|---|---|
| Metal | 147–165 |
| CPU | ~20 |

### 性能 — op 微基准（bench_ops 节选）

| op | Metal | CPU |
|---|---|---|
| gemv q8_0 32000×4096 | 0.84 ms / 628 GB/s | 8.47 ms / 62 GB/s |
| gemv q4_0 32000×4096 | 0.53 ms / 983 GB/s | 8.59 ms / 61 GB/s |
| gemv q4_0 151936×1024 | 0.75 ms / 834 GB/s | 3.69 ms / 169 GB/s |
| attention seq=2048 | 0.42 ms | 31.79 ms |
| attention seq=4096 | 0.75 ms | 79.68 ms |

### 性能 — 端到端（CLI, warm 中位数）

| 用例 | Metal prefill | Metal decode | CPU prefill | CPU decode | prefill 加速 |
|---|---|---|---|---|---|
| OCR doc_small (197 tok) | 450–491 ms | ~97 ms /16 tok | 9821 ms | 1201 ms /16 tok | **21x** |
| OCR doc_large (869 tok) | 2386–2480 ms | ~123 ms /16 tok | 93790 ms | 2473 ms /16 tok | **39x** |
| 纯文本长 prompt (175 tok) | 63–88 ms | 159–165 tok/s | 2904 ms | 14 tok/s | **~40x** |

### 与上一版（HEAD `8d624895a`）对比

| 用例 | HEAD | 本版 | 提升 |
|---|---|---|---|
| OCR 大图 prefill (3136 patches 噪音图, 1264 tok) | 21300 ms | ~5200 ms | **4.1x**（MPS-tiled attention） |
| OCR doc_small prefill | 1022 ms | ~460 ms | **2.2x** |
| OCR doc_large prefill | 2970 ms（chunk 64 中间态） | ~2400 ms | **1.24x**（chunk 256） |
| Metal OCR 正确性 | 全 `<unk>`/乱码（两个 bug） | 与 CPU 字节一致 | — |

### 已知良性差异

- 纯文本短 prompt 在第 ~24 token 后 CPU/Metal 分叉：Metal MatMul 走 f16 GEMM，logits 接近时 argmax 翻转属预期精度漂移，与 HEAD 行为一致。
- 首轮运行（Metal shader 冷编译）prefill 慢 ~1.2-1.4x，性能数字取预热后运行。
