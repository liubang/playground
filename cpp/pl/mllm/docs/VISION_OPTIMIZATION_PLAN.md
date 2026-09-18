# mllm OCR 推理优化路线图

> 2026-09-18 起。目标：在不损失正确性的前提下提升 OCR（PaddleOCR-VL）端到端推理效率。
> 每级优化都必须用数据说话：正确性以 `e2e_golden_ocr`（stdout SHA-256 + 语义断言）为准，
> 效率以 `bench_ops` / `bench_decode` / CLI warm 中位数为准，结果回写 PERF_BASELINE.md。

## 瓶颈定位（基线 2026-09-13，M4 Pro，doc_large 869 tok）

vision tower 分层 profile（总 1706 ms，占 OCR 大图端到端 ~85%）：

| stage    | ms    | 占比 | 备注                              |
| -------- | ----- | ---- | --------------------------------- |
| blk_attn | 760.3 | 45%  | fused f32 flash kernel，~1.7 TFLOP/s |
| blk_mlp  | 543.6 | 32%  | f16 MPS GEMM（~4 TFLOP/s）+ GELU  |
| blk_qkv  | 264.8 | 16%  | LN + 3x GEMM + 3x bias + rope     |
| blk_oproj| 88.5  | 5%   |                                   |
| 其余     | <50   | ~2%  | patch_embed / rope_tables / merge |

decode ~160 tok/s，对 OCR 短输出（48 tok ≈ 0.3 s）不是瓶颈。优化主战场 = vision tower。

## A 级：逐位等价，零正确性风险（golden hash 不变）

### A1. 共享输入的批量 MatMul 融合（MatMulFused batch>1 快速路径）

现状：vision QKV 是 3 次独立 `MatMul`，f16 MPS 路径每次都要把同一份 LN 输出
`x`（f32 → f16）重新转换一遍，3 次转换冗余 2 次；LM prefill 的 QKV / gate-up
同理（`TransformerLayer::ForwardBatch` 也走 `MatMulFused`，但 Metal 端 batch>1
直接退化为逐次 `MatMul`）。

方案：`MetalBackend::MatMulFused` 增加 batch>1 的 f16 快速路径——上传 + 转换
输入一次，随后在同一 deferred command buffer 里连续 encode N 个 MPS GEMM
（每个输出各自的 cvt f16→f32）。不满足条件（f32 / 量化 / 维度不对齐）时维持
原逐次回退，行为不变。vision tower 的 QKV 三调用改为一次 `MatMulFused`。

逐位等价论证：每个 GEMM 的 shape、输入数据、kernel 与转换 kernel 均与独立调用
相同，仅省去重复的输入转换（转换是确定性的逐元素映射，重复三次与一次结果一致）。

### A2. fused vision attention 的 threadgroup  staging（v6）

现状（v5）：每个 threadgroup 2 个 simdgroup 各自对全部 K/V 做 strided
`simdgroup_load`（行距 `num_heads*head_dim`），每个 32-key block 每 simdgroup
36+36 次设备加载 vs 72 次 simdgroup MAC —— 加载发射率瓶颈，实测 ~1.7 TFLOP/s
（f32），且两个 simdgroup 的 K/V 加载完全冗余。

方案（v6）：每个 key block 由整个 threadgroup（64 线程）先把 K/V tile
（32 x head_dim f32）以合并加载搬进 threadgroup memory，两个 simdgroup 再从
threadgroup memory 取 fragment。消除冗余 + 设备 strided 加载移出内层循环。

逐位等价论证：staging 是纯数据搬运（含与 v5 相同的尾部分块 clamp 语义），
每个输出元素的 simdgroup_multiply(_accumulate) 序列、online softmax 分块
（BN=32）、rescale 时机完全不变。

> 落地修正（2026-09-18）：v6 实测为负结果（threadgroup memory 4.9KB → 23.7KB
> 拖垮 occupancy，见文末验收结果）。实际合入的是等价替代方案 **K/V 预转置**：
> host 侧新增 `mllm_kv_transpose` kernel，在 n ≥ 1024 时把 K/V 从交错布局
> `[n, heads, hd]` 转成按 head 连续的 `[heads, n, hd]` 平面，fused kernel 的
> fragment 加载行距从 `num_heads*head_dim` 缩到 `head_dim`（纯数据搬运，
> 逐位等价论证同上）；kernel 通过 `kv_stride` 参数兼容两种布局，小 n 维持
> 原交错路径。实测 attn_full n=3456 提速 1.14x。

### A3. host 预处理并行化（patchify / resize，可选）

patchify ~21 ms（doc_large），单线程 CPU。多线程化为 embarrassingly parallel，
逐位等价。占比 ~1%，视 A1/A2 验收后再定。

## B 级：数值微差、OCR 输出几乎必然不变（需重新冻结 golden + 加强语义断言）

### B1. vision attention f16 输入 + f32 累加

Q/K/V 转 f16 做 simdgroup 矩阵乘（Apple GPU f16 矩阵吞吐为 f32 两倍），softmax
与 O 累加保持 f32。blk_attn 760 ms 预计 → ~400 ms。llama.cpp vision tower 同款
做法。注意：vision GEMM 已经是 f16 MPS（见下），attention 是仅剩的 f32 大户。

### B2. 现状备忘：vision GEMM 已是 f16

mmproj 的 BF16 权重在导入时已转 f16，MatMul 走 f16 MPS GEMM（in-repo
~4 TFLOP/s），输入激活 f32↔f16 转换 kernel 仅占 ~2%。剩余提升空间在自研
simdgroup GEMM（SPEC §13.2 暂缓项），需单独设计。

### B3. LM 量化（Q8_0/Q4_0）

decode 带宽收益 ~1.6x，但 decode 仅占 OCR 端到端 ~5%，优先级低。

## C 级：换模型 / 改架构（工作量大，收益不确定，暂缓）

- DeepSeek-OCR：vision token 压缩比高（一页 ~100-256 tok vs 869），但视觉编码器
  FLOPs 更高、decoder 为 3B MoE（SPEC 明确暂不做），需新写 vision tower + MoE。
- vision token 剪枝 / 降输入分辨率：直接有损，违反前提，排除。

## 执行顺序

1. A1 + A2（本轮）：要求 golden hash 逐字节不变 + bench_ops attn_full / CLI 提速。
2. B1（下一轮）：golden 改为语义断言为主，重新冻结 hash。
3. B3 / C 级：视前面收益再评估。

## A 级验收结果（2026-09-18，已落地）

**正确性**：单元测试 15/15；e2e_golden_ocr 6/6，四用例 stdout SHA-256 与
2026-09-13 冻结 golden 逐字节相同（Metal + CPU 跨后端一致；text_short CPU
WARN 为先前已记录的良性 f16 漂移）。

**效率**：attn_full n=3456 32.5 → 28.4-28.7 ms（1.14x）；vision tower
1706 → 1515 ms（-11.2%）；端到端 OCR prefill doc_small ~1.06x /
doc_large ~1.07x。详见 PERF_BASELINE.md 基线 2026-09-18。

**负结果**（试错记录，未合入）：

- threadgroup K/V staging（v6）：tg memory 4.9KB → 23.7KB，occupancy
  ~6 → 1 threadgroup/core，慢 3 倍（0.53-0.56 TFLOP/s）。
- 16-query-row 寄存器分块（fused16）：寄存器压力，慢 3 倍
  （0.63-0.67 TFLOP/s）。

结论：v5 的 4.9KB / 6-tg occupancy 是 f32 fused kernel 的性能关键；
A 级空间已基本到顶（端到端 ~1.07x），下一个数量级收益在 B1（f16
attention，blk_attn 占 vision 45%，预计 vision 再提速 ~1.3-1.5x）。
