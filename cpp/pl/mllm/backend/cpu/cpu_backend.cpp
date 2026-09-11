// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)
// Created: 2026/08/29 22:15

#include "cpp/pl/mllm/backend/cpu/cpu_backend.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

#include "cpp/pl/mllm/core/dtype.h"

namespace pl::mllm {

namespace {

// Quantized block layouts (ggml-compatible, little-endian)

struct Q8Block {
    uint16_t scale; // fp16
    int8_t qs[32];
};
static_assert(sizeof(Q8Block) == kQ8_0TypeSize);

// Q4_0 block: fp16 scale + 16 packed 4-bit values (ggml layout).
// Elements 0..15 come from the low nibbles of qs[0..15], elements 16..31
// from the high nibbles; each value is (nibble - 8) * scale.
struct Q4Block {
    uint16_t scale; // fp16
    uint8_t qs[16];
};
static_assert(sizeof(Q4Block) == kQ4_0TypeSize);

// Validation helpers

Status check_contiguous(const TensorView& t, std::string_view op) {
    if (!t.valid()) {
        return Status::Error(ErrorCode::kInvalidArgument, std::string(op) + ": invalid tensor");
    }
    if (!t.is_contiguous()) {
        return Status::Error(ErrorCode::kUnsupported, std::string(op) + ": non-contiguous tensor");
    }
    return {};
}

// Convert a TensorView element to f32 (handles quantized by dequantizing the
// whole block the element belongs to — used only for scalar debug access).
float elem_to_f32(const void* base, DType dtype, int64_t idx) {
    switch (dtype) {
        case DType::kF32:
            return static_cast<const float*>(base)[static_cast<size_t>(idx)];
        case DType::kF16:
            return fp16_to_fp32(static_cast<const uint16_t*>(base)[static_cast<size_t>(idx)]);
        case DType::kBF16:
            return bf16_to_fp32(static_cast<const uint16_t*>(base)[static_cast<size_t>(idx)]);
        case DType::kQ8_0: {
            const auto* blocks = static_cast<const Q8Block*>(base);
            const int64_t block_idx = idx / kQ8_0BlockSize;
            const int64_t in_block = idx % kQ8_0BlockSize;
            const float scale = fp16_to_fp32(blocks[block_idx].scale);
            return scale * static_cast<float>(blocks[block_idx].qs[in_block]);
        }
        case DType::kQ4_0: {
            const auto* blocks = static_cast<const Q4Block*>(base);
            const int64_t block_idx = idx / kQ4_0BlockSize;
            const int64_t in_block = idx % kQ4_0BlockSize;
            const float scale = fp16_to_fp32(blocks[block_idx].scale);
            // j in [0,16): low nibble -> element j; high nibble -> element j+16.
            const uint8_t packed = blocks[block_idx].qs[in_block % 16];
            const int32_t nibble = (in_block < 16) ? (packed & 0x0F) : (packed >> 4);
            return scale * static_cast<float>(nibble - 8);
        }
    }
    return 0.0f;
}

// MatMul kernels

// Output rows of a MatMul (over the [out_dim] of every batch) are
// independent GEMVs, so they parallelize perfectly with no synchronization.
// Splits [0, n_rows) across the hardware threads, running one range inline
// on the caller thread; falls back to a single call for tiny workloads.
template <typename F> void parallel_for_rows(int32_t n_rows, F&& fn) {
    const unsigned hw = std::thread::hardware_concurrency();
    const int32_t max_threads = hw == 0 ? 1 : static_cast<int32_t>(hw);
    // Keep each task's work meaningful: the vision encoder and the LM head
    // easily saturate every core; tiny projections stay single-threaded.
    const int32_t n_tasks = std::min(max_threads, std::max(1, n_rows / 64));
    if (n_tasks <= 1) {
        fn(0, n_rows);
        return;
    }
    const int32_t chunk = (n_rows + n_tasks - 1) / n_tasks;
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(n_tasks - 1));
    for (int32_t t = 1; t < n_tasks; ++t) {
        const int32_t begin = chunk * t;
        const int32_t end = std::min(n_rows, begin + chunk);
        if (begin < end) {
            pool.emplace_back([&fn, begin, end] { fn(begin, end); });
        }
    }
    fn(0, std::min(n_rows, chunk));
    for (auto& th : pool) {
        th.join();
    }
}

// Convert one x element (f32 or f16) to float.
template <typename XT> inline float xt_to_f32(XT v) {
    if constexpr (std::is_same_v<XT, uint16_t>) {
        return fp16_to_fp32(v);
    } else {
        return static_cast<float>(v);
    }
}

// GEMV/GEMM with bf16 weights: out[b, o] = sum_i x[b, i] * w[o, i]
// (w stored as [out_dim, in_dim]). x is f32; bf16 inputs do not occur on
// the activation path. Writes the output rows [o_begin, o_end) of every
// batch row. Each weight row is converted to f32 ONCE per output row and
// reused across batch rows: the old b-outer order paid a bf16->f32
// conversion per MAC, which dominated the kernel for batched prefill.
inline void matmul_bf16(float* out,
                        const float* x,
                        const uint16_t* w,
                        int32_t batch,
                        int32_t out_dim,
                        int32_t in_dim,
                        int32_t o_begin,
                        int32_t o_end) {
    thread_local std::vector<float> wrow;
    wrow.resize(static_cast<size_t>(in_dim));
    float* wf = wrow.data();
    for (int32_t o = o_begin; o < o_end; ++o) {
        const uint16_t* wr = w + static_cast<size_t>(o) * static_cast<size_t>(in_dim);
        for (int32_t i = 0; i < in_dim; ++i) {
            wf[i] = bf16_to_fp32(wr[i]);
        }
        for (int32_t b = 0; b < batch; ++b) {
            const float* xb = x + static_cast<size_t>(b) * static_cast<size_t>(in_dim);
            // 8 independent partial sums break the FP reduction dependency
            // chain (compiler may not reassociate fp reductions): without
            // them every MAC waits on the previous FMA's latency.
            float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
            float s4 = 0.0f, s5 = 0.0f, s6 = 0.0f, s7 = 0.0f;
            int32_t i = 0;
            for (; i + 8 <= in_dim; i += 8) {
                s0 += xb[i] * wf[i];
                s1 += xb[i + 1] * wf[i + 1];
                s2 += xb[i + 2] * wf[i + 2];
                s3 += xb[i + 3] * wf[i + 3];
                s4 += xb[i + 4] * wf[i + 4];
                s5 += xb[i + 5] * wf[i + 5];
                s6 += xb[i + 6] * wf[i + 6];
                s7 += xb[i + 7] * wf[i + 7];
            }
            s0 += ((s1 + s2) + (s3 + s4)) + ((s5 + s6) + s7);
            for (; i < in_dim; ++i) {
                s0 += xb[i] * wf[i];
            }
            out[static_cast<size_t>(b) * static_cast<size_t>(out_dim) + static_cast<size_t>(o)] =
                s0;
        }
    }
}

// out[b, o] = sum_i x[b, i] * w[o, i]   (w stored as [out_dim, in_dim])
// XT/WT are the element types of x/weight; both convert to float for the MAC.
// Writes the output rows [o_begin, o_end) of every batch row.
template <typename XT, typename WT>
void matmul_fxx(float* out,
                const XT* x,
                const WT* w,
                int32_t batch,
                int32_t out_dim,
                int32_t in_dim,
                int32_t o_begin,
                int32_t o_end) {
    for (int32_t b = 0; b < batch; ++b) {
        const XT* xb = x + static_cast<size_t>(b) * static_cast<size_t>(in_dim);
        float* ob = out + static_cast<size_t>(b) * static_cast<size_t>(out_dim);
        for (int32_t o = o_begin; o < o_end; ++o) {
            const WT* wr = w + static_cast<size_t>(o) * static_cast<size_t>(in_dim);
            float acc = 0.0f;
            for (int32_t i = 0; i < in_dim; ++i) {
                if constexpr (std::is_same_v<WT, uint16_t>) {
                    acc += static_cast<float>(xb[i]) * fp16_to_fp32(wr[i]);
                } else {
                    acc += static_cast<float>(xb[i]) * static_cast<float>(wr[i]);
                }
            }
            ob[o] = acc;
        }
    }
}

// Fused Q8_0 GEMV: x is f32/f16, weight is Q8_0 blocks.
// Writes the output rows [o_begin, o_end) of every batch row.
template <typename XT>
void matmul_q8_0(float* out,
                 const XT* x,
                 const Q8Block* w,
                 int32_t batch,
                 int32_t out_dim,
                 int32_t in_dim,
                 int32_t o_begin,
                 int32_t o_end) {
    constexpr int64_t block = kQ8_0BlockSize;
    const int32_t num_blocks = in_dim / static_cast<int32_t>(block);

    for (int32_t b = 0; b < batch; ++b) {
        const XT* xb = x + static_cast<size_t>(b) * static_cast<size_t>(in_dim);
        float* ob = out + static_cast<size_t>(b) * static_cast<size_t>(out_dim);
        for (int32_t o = o_begin; o < o_end; ++o) {
            const Q8Block* wr = w + static_cast<size_t>(o) * static_cast<size_t>(num_blocks);
            float acc = 0.0f;
            for (int32_t blk = 0; blk < num_blocks; ++blk) {
                const float scale = fp16_to_fp32(wr[blk].scale);
                float dot = 0.0f;
                for (int j = 0; j < block; ++j) {
                    dot += static_cast<float>(xb[blk * static_cast<int32_t>(block) + j]) *
                           static_cast<float>(wr[blk].qs[j]);
                }
                acc += dot * scale;
            }
            ob[o] = acc;
        }
    }
}

// Fused Q4_0 GEMV: x is f32/f16, weight is Q4_0 blocks.
// Writes the output rows [o_begin, o_end) of every batch row.
template <typename XT>
void matmul_q4_0(float* out,
                 const XT* x,
                 const Q4Block* w,
                 int32_t batch,
                 int32_t out_dim,
                 int32_t in_dim,
                 int32_t o_begin,
                 int32_t o_end) {
    constexpr int64_t block = kQ4_0BlockSize;
    const int32_t num_blocks = in_dim / static_cast<int32_t>(block);

    for (int32_t b = 0; b < batch; ++b) {
        const XT* xb = x + static_cast<size_t>(b) * static_cast<size_t>(in_dim);
        float* ob = out + static_cast<size_t>(b) * static_cast<size_t>(out_dim);
        for (int32_t o = o_begin; o < o_end; ++o) {
            const Q4Block* wr = w + static_cast<size_t>(o) * static_cast<size_t>(num_blocks);
            float acc = 0.0f;
            for (int32_t blk = 0; blk < num_blocks; ++blk) {
                const float scale = fp16_to_fp32(wr[blk].scale);
                const int32_t base = blk * static_cast<int32_t>(block);
                float dot = 0.0f;
                // Low nibbles cover elements [0,16), high nibbles [16,32).
                for (int j = 0; j < 16; ++j) {
                    const int32_t lo = wr[blk].qs[j] & 0x0F;
                    const int32_t hi = wr[blk].qs[j] >> 4;
                    dot += xt_to_f32(xb[base + j]) * static_cast<float>(lo - 8) +
                           xt_to_f32(xb[base + j + 16]) * static_cast<float>(hi - 8);
                }
                acc += dot * scale;
            }
            ob[o] = acc;
        }
    }
}

// RoPE helpers

// Compute theta_i for dim i: 1 / (freq_base ^ (2i/head_dim))
inline float rope_theta(int32_t i, int32_t head_dim, float freq_base) {
    return 1.0f / std::pow(freq_base, static_cast<float>(2 * i) / static_cast<float>(head_dim));
}

// Apply RoPE in-place to a single head of `head_dim` elements.
// LLaMA / Qwen use GPT-NeoX style "half-split" rotation: the second half of
// the head is paired with the first half (NOT the interleaved GPT-J scheme):
//   x'[i]        = x[i] * cos(p*theta_i) - x[i + d/2] * sin(p*theta_i)
//   x'[i + d/2]  = x[i] * sin(p*theta_i) + x[i + d/2] * cos(p*theta_i)
//   theta_i      = freq_base^(-2i/head_dim)
template <typename T>
void apply_rope_head(T* ptr, int32_t head_dim, int64_t position, float freq_base) {
    const float p = static_cast<float>(position);
    const int32_t half = head_dim / 2;
    for (int32_t i = 0; i < half; ++i) {
        const float theta = rope_theta(i, head_dim, freq_base);
        const float angle = p * theta;
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        const float a = static_cast<float>(ptr[i]);
        const float b = static_cast<float>(ptr[i + half]);
        ptr[i] = static_cast<T>(a * c - b * s);
        ptr[i + half] = static_cast<T>(a * s + b * c);
    }
}

template <typename T>
void apply_rope(TensorView q,
                TensorView k,
                int64_t position,
                int32_t head_dim,
                int32_t num_heads,
                int32_t num_kv_heads,
                float freq_base) {
    const int32_t batch = static_cast<int32_t>(q.shape().dim(0));
    auto* qd = q.data_as<T>();
    auto* kd = k.data_as<T>();
    const int32_t q_stride = num_heads * head_dim;
    const int32_t k_stride = num_kv_heads * head_dim;

    for (int32_t b = 0; b < batch; ++b) {
        // Batch semantics: row b belongs to sequence position `position + b`
        // (single-token decode passes batch == 1 with the absolute position).
        T* qb = qd + static_cast<size_t>(b) * static_cast<size_t>(q_stride);
        T* kb = kd + static_cast<size_t>(b) * static_cast<size_t>(k_stride);
        for (int32_t h = 0; h < num_heads; ++h) {
            apply_rope_head(qb + h * head_dim, head_dim, position + b, freq_base);
        }
        for (int32_t h = 0; h < num_kv_heads; ++h) {
            apply_rope_head(kb + h * head_dim, head_dim, position + b, freq_base);
        }
    }
}

} // namespace

// CpuBackend implementation

Status CpuBackend::ImportWeights(std::span<const TensorView> weights,
                                 std::span<const std::string_view> names) {
    if (weights.size() != names.size()) {
        return Status::Error(ErrorCode::kInvalidArgument, "ImportWeights: size mismatch");
    }
    for (size_t i = 0; i < weights.size(); ++i) {
        if (!weights[i].valid()) {
            return Status::Error(ErrorCode::kInvalidArgument,
                                 "ImportWeights: invalid weight tensor");
        }
        weights_.emplace(std::string(names[i]), weights[i]);
    }
    return {};
}

const TensorView* CpuBackend::FindWeight(std::string_view name) const {
    auto it = weights_.find(name);
    return it != weights_.end() ? &it->second : nullptr;
}

// MatMul

Status CpuBackend::MatMul(TensorView out, TensorView x, std::string_view weight_name) {
    if (auto s = check_contiguous(out, "MatMul"); !s.ok())
        return s;
    if (auto s = check_contiguous(x, "MatMul"); !s.ok())
        return s;

    auto it = weights_.find(weight_name);
    if (it == weights_.end()) {
        return Status::Error(ErrorCode::kNotFound,
                             "MatMul: weight '" + std::string(weight_name) + "' not found");
    }
    const TensorView& w = it->second;

    // x: [batch, in_dim], w: [out_dim, in_dim], out: [batch, out_dim]
    const int32_t batch = static_cast<int32_t>(x.shape().dim(0));
    const int32_t in_dim = static_cast<int32_t>(x.shape().dim(1));
    const int32_t out_dim = static_cast<int32_t>(w.shape().dim(0));

    if (out.shape().dim(0) != batch || out.shape().dim(1) != out_dim) {
        return Status::Error(ErrorCode::kInvalidArgument, "MatMul: output shape mismatch");
    }
    if (w.shape().dim(1) != in_dim) {
        return Status::Error(ErrorCode::kInvalidArgument, "MatMul: in_dim mismatch");
    }
    if (out.dtype() != DType::kF32) {
        return Status::Error(ErrorCode::kUnsupported, "MatMul: output must be f32 (CPU debug)");
    }

    auto* out_ptr = out.data_as<float>();

    if (w.dtype() == DType::kQ4_0) {
        if (in_dim % kQ4_0BlockSize != 0) {
            return Status::Error(ErrorCode::kInvalidArgument,
                                 "MatMul: in_dim not Q4_0 block-aligned");
        }
        const auto* wq = static_cast<const Q4Block*>(w.data());
        if (x.dtype() == DType::kF32) {
            const auto* xp = x.data_as<float>();
            parallel_for_rows(out_dim, [&](int32_t ob, int32_t oe) {
                matmul_q4_0(out_ptr, xp, wq, batch, out_dim, in_dim, ob, oe);
            });
        } else if (x.dtype() == DType::kF16) {
            const auto* xp = x.data_as<uint16_t>();
            parallel_for_rows(out_dim, [&](int32_t ob, int32_t oe) {
                matmul_q4_0(out_ptr, xp, wq, batch, out_dim, in_dim, ob, oe);
            });
        } else {
            return Status::Error(ErrorCode::kUnsupported,
                                 "MatMul: Q4_0 weight requires f32/f16 input");
        }
        return {};
    }
    if (w.dtype() == DType::kQ8_0) {
        if (in_dim % kQ8_0BlockSize != 0) {
            return Status::Error(ErrorCode::kInvalidArgument,
                                 "MatMul: in_dim not Q8_0 block-aligned");
        }
        const auto* wq = static_cast<const Q8Block*>(w.data());
        if (x.dtype() == DType::kF32) {
            const auto* xp = x.data_as<float>();
            parallel_for_rows(out_dim, [&](int32_t ob, int32_t oe) {
                matmul_q8_0(out_ptr, xp, wq, batch, out_dim, in_dim, ob, oe);
            });
        } else if (x.dtype() == DType::kF16) {
            const auto* xp = x.data_as<uint16_t>();
            parallel_for_rows(out_dim, [&](int32_t ob, int32_t oe) {
                matmul_q8_0(out_ptr, xp, wq, batch, out_dim, in_dim, ob, oe);
            });
        } else {
            return Status::Error(ErrorCode::kUnsupported,
                                 "MatMul: Q8_0 weight requires f32/f16 input");
        }
    } else if (w.dtype() == DType::kF32) {
        if (x.dtype() == DType::kF32) {
            const auto* xp = x.data_as<float>();
            const auto* wp = w.data_as<float>();
            parallel_for_rows(out_dim, [&](int32_t ob, int32_t oe) {
                matmul_fxx<float, float>(out_ptr, xp, wp, batch, out_dim, in_dim, ob, oe);
            });
        } else if (x.dtype() == DType::kF16) {
            const auto* xp = x.data_as<uint16_t>();
            const auto* wp = w.data_as<float>();
            parallel_for_rows(out_dim, [&](int32_t ob, int32_t oe) {
                matmul_fxx<uint16_t, float>(out_ptr, xp, wp, batch, out_dim, in_dim, ob, oe);
            });
        } else {
            return Status::Error(ErrorCode::kUnsupported, "MatMul: unsupported input dtype");
        }
    } else if (w.dtype() == DType::kF16) {
        if (x.dtype() == DType::kF32) {
            const auto* xp = x.data_as<float>();
            const auto* wp = w.data_as<uint16_t>();
            parallel_for_rows(out_dim, [&](int32_t ob, int32_t oe) {
                matmul_fxx<float, uint16_t>(out_ptr, xp, wp, batch, out_dim, in_dim, ob, oe);
            });
        } else if (x.dtype() == DType::kF16) {
            const auto* xp = x.data_as<uint16_t>();
            const auto* wp = w.data_as<uint16_t>();
            parallel_for_rows(out_dim, [&](int32_t ob, int32_t oe) {
                matmul_fxx<uint16_t, uint16_t>(out_ptr, xp, wp, batch, out_dim, in_dim, ob, oe);
            });
        } else {
            return Status::Error(ErrorCode::kUnsupported, "MatMul: unsupported input dtype");
        }
    } else if (w.dtype() == DType::kBF16) {
        if (x.dtype() == DType::kF32) {
            const auto* xp = x.data_as<float>();
            const auto* wp = w.data_as<uint16_t>();
            parallel_for_rows(out_dim, [&](int32_t ob, int32_t oe) {
                matmul_bf16(out_ptr, xp, wp, batch, out_dim, in_dim, ob, oe);
            });
        } else {
            return Status::Error(ErrorCode::kUnsupported, "MatMul: BF16 weight requires f32 input");
        }
    } else {
        return Status::Error(ErrorCode::kUnsupported, "MatMul: unsupported weight dtype");
    }

    return {};
}

// RMSNorm

Status CpuBackend::RmsNorm(TensorView out, TensorView x, TensorView weight, float eps) {
    if (auto s = check_contiguous(out, "RmsNorm"); !s.ok())
        return s;
    if (auto s = check_contiguous(x, "RmsNorm"); !s.ok())
        return s;
    if (auto s = check_contiguous(weight, "RmsNorm"); !s.ok())
        return s;

    const int32_t batch = static_cast<int32_t>(x.shape().dim(0));
    const int32_t hidden = static_cast<int32_t>(x.shape().dim(1));

    if (out.shape() != x.shape() || weight.shape().numel() != hidden) {
        return Status::Error(ErrorCode::kInvalidArgument, "RmsNorm: shape mismatch");
    }
    if (!(eps > 0.0f)) {
        return Status::Error(ErrorCode::kInvalidArgument, "RmsNorm: eps must be positive");
    }
    if (out.dtype() != DType::kF32) {
        return Status::Error(ErrorCode::kUnsupported, "RmsNorm: output must be f32 (CPU debug)");
    }

    auto* od = out.data_as<float>();
    const float inv = 1.0f / static_cast<float>(hidden);

    for (int32_t b = 0; b < batch; ++b) {
        // mean of squares
        float ms = 0.0f;
        for (int32_t i = 0; i < hidden; ++i) {
            const float v = elem_to_f32(x.data(), x.dtype(), static_cast<int64_t>(b) * hidden + i);
            ms += v * v;
        }
        ms *= inv;
        const float denom = 1.0f / std::sqrt(ms + eps);

        for (int32_t i = 0; i < hidden; ++i) {
            const float xv = elem_to_f32(x.data(), x.dtype(), static_cast<int64_t>(b) * hidden + i);
            const float wv = elem_to_f32(weight.data(), weight.dtype(), i);
            od[static_cast<size_t>(b) * static_cast<size_t>(hidden) + static_cast<size_t>(i)] =
                xv * denom * wv;
        }
    }
    return {};
}

// RoPE

Status CpuBackend::RoPE(TensorView q, TensorView k, int64_t position, const RopeConfig& config) {
    if (auto s = check_contiguous(q, "RoPE"); !s.ok())
        return s;
    if (auto s = check_contiguous(k, "RoPE"); !s.ok())
        return s;

    if (q.shape().rank() != 3 || k.shape().rank() != 3) {
        return Status::Error(ErrorCode::kInvalidArgument,
                             "RoPE: expected 3D tensors [batch, heads, head_dim]");
    }
    const int32_t num_heads = static_cast<int32_t>(q.shape().dim(1));
    const int32_t num_kv_heads = static_cast<int32_t>(k.shape().dim(1));
    const int32_t head_dim =
        config.head_dim > 0 ? config.head_dim : static_cast<int32_t>(q.shape().dim(2));

    if (q.shape().dim(2) != head_dim || k.shape().dim(2) != head_dim) {
        return Status::Error(ErrorCode::kInvalidArgument, "RoPE: head_dim mismatch");
    }
    if (position < 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "RoPE: negative position");
    }
    if (head_dim % 2 != 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "RoPE: head_dim must be even");
    }

    if (q.dtype() != k.dtype()) {
        return Status::Error(ErrorCode::kInvalidArgument, "RoPE: q/k dtype mismatch");
    }

    // Optional per-head Q/K RMSNorm (Qwen3 family) applied before rotation.
    // Matches standalone RmsNorm math exactly (serial accumulation, 1/sqrt).
    const auto apply_qk_norm = [&](TensorView t, int32_t heads, TensorView w) -> Status {
        if (t.dtype() != DType::kF32 || w.dtype() != DType::kF32) {
            return Status::Error(ErrorCode::kUnsupported,
                                 "RoPE qk_norm: f32 required (CPU reference)");
        }
        float* d = t.data_as<float>();
        const float* wd = w.data_as<const float>();
        const float inv = 1.0f / static_cast<float>(head_dim);
        for (int32_t h = 0; h < heads; ++h) {
            float* row = d + static_cast<size_t>(h) * static_cast<size_t>(head_dim);
            float ms = 0.0f;
            for (int32_t i = 0; i < head_dim; ++i) {
                ms += row[i] * row[i];
            }
            ms *= inv;
            const float denom = 1.0f / std::sqrt(ms + config.rms_eps);
            for (int32_t i = 0; i < head_dim; ++i) {
                row[i] = row[i] * denom * wd[i];
            }
        }
        return {};
    };
    if (config.q_norm.valid()) {
        // q reshaped view: rows are (batch * heads); batch==1 in decode, but
        // flatten the convention: every row of [batch, heads, hd] gets normed.
        const int32_t rows = static_cast<int32_t>(q.shape().dim(0)) * num_heads;
        const int32_t krows = static_cast<int32_t>(k.shape().dim(0)) * num_kv_heads;
        auto q3 = q.reshape({rows, head_dim});
        auto k3 = k.reshape({krows, head_dim});
        if (!q3.ok())
            return q3.status();
        if (!k3.ok())
            return k3.status();
        if (auto s = apply_qk_norm(q3.value(), rows, config.q_norm); !s.ok())
            return s;
        if (auto s = apply_qk_norm(k3.value(), krows, config.k_norm); !s.ok())
            return s;
    }

    if (q.dtype() == DType::kF32) {
        apply_rope<float>(q, k, position, head_dim, num_heads, num_kv_heads, config.freq_base);
    } else if (q.dtype() == DType::kF16) {
        apply_rope<uint16_t>(q, k, position, head_dim, num_heads, num_kv_heads, config.freq_base);
    } else {
        return Status::Error(ErrorCode::kUnsupported, "RoPE: unsupported dtype");
    }
    return {};
}

// Attention

Status CpuBackend::Attention(TensorView out,
                             TensorView q,
                             const KVCacheView& kv,
                             const AttentionConfig& config) {
    if (auto s = check_contiguous(out, "Attention"); !s.ok())
        return s;
    if (auto s = check_contiguous(q, "Attention"); !s.ok())
        return s;

    // q: [1, num_heads, head_dim]
    if (q.shape().rank() != 3 || q.shape().dim(0) != 1) {
        return Status::Error(ErrorCode::kInvalidArgument,
                             "Attention: q must be [1, num_heads, head_dim]");
    }
    const int32_t num_heads = config.num_heads;
    const int32_t num_kv_heads = config.num_kv_heads;
    const int32_t head_dim = config.head_dim;
    const int32_t group_size = num_heads / num_kv_heads;
    const int32_t seq_len = kv.seq_len;
    const float scale =
        config.scale > 0.0f ? config.scale : 1.0f / std::sqrt(static_cast<float>(head_dim));

    if (kv.keys == nullptr || kv.values == nullptr) {
        return Status::Error(ErrorCode::kInvalidArgument, "Attention: null KV cache");
    }
    if (kv.num_kv_heads != num_kv_heads || kv.head_dim != head_dim) {
        return Status::Error(ErrorCode::kInvalidArgument, "Attention: KV cache shape mismatch");
    }
    if (q.shape().dim(1) != num_heads || q.shape().dim(2) != head_dim) {
        return Status::Error(ErrorCode::kInvalidArgument, "Attention: q head config mismatch");
    }
    if (out.shape().dim(0) != 1 || out.shape().dim(1) != num_heads * head_dim) {
        return Status::Error(ErrorCode::kInvalidArgument, "Attention: output shape mismatch");
    }
    if (out.dtype() != DType::kF32) {
        return Status::Error(ErrorCode::kUnsupported, "Attention: output must be f32 (CPU debug)");
    }

    auto* od = out.data_as<float>();

    for (int32_t h = 0; h < num_heads; ++h) {
        const int32_t kv_head = h / group_size;

        // Compute attention scores: q · k_j for j = 0..seq_len-1
        std::vector<float> scores(static_cast<size_t>(seq_len));
        float max_score = -std::numeric_limits<float>::infinity();

        for (int32_t j = 0; j < seq_len; ++j) {
            float dot = 0.0f;
            for (int32_t d = 0; d < head_dim; ++d) {
                const float qv =
                    elem_to_f32(q.data(), q.dtype(), static_cast<int64_t>(h) * head_dim + d);
                const float kv_ = elem_to_f32(kv.keys,
                                              kv.dtype,
                                              static_cast<int64_t>(j) * num_kv_heads * head_dim +
                                                  static_cast<int64_t>(kv_head) * head_dim + d);
                dot += qv * kv_;
            }
            scores[static_cast<size_t>(j)] = dot * scale;
            if (scores[static_cast<size_t>(j)] > max_score) {
                max_score = scores[static_cast<size_t>(j)];
            }
        }

        // Softmax
        float sum_exp = 0.0f;
        for (int32_t j = 0; j < seq_len; ++j) {
            scores[static_cast<size_t>(j)] = std::exp(scores[static_cast<size_t>(j)] - max_score);
            sum_exp += scores[static_cast<size_t>(j)];
        }
        const float inv_sum = 1.0f / sum_exp;

        // Weighted sum of V
        for (int32_t d = 0; d < head_dim; ++d) {
            float acc = 0.0f;
            for (int32_t j = 0; j < seq_len; ++j) {
                const float vv = elem_to_f32(kv.values,
                                             kv.dtype,
                                             static_cast<int64_t>(j) * num_kv_heads * head_dim +
                                                 static_cast<int64_t>(kv_head) * head_dim + d);
                acc += scores[static_cast<size_t>(j)] * vv * inv_sum;
            }
            od[static_cast<size_t>(h) * static_cast<size_t>(head_dim) + static_cast<size_t>(d)] =
                acc;
        }
    }
    return {};
}

// SwiGLU

Status CpuBackend::SwiGLU(TensorView out, TensorView gate, TensorView up) {
    if (auto s = check_contiguous(out, "SwiGLU"); !s.ok())
        return s;
    if (auto s = check_contiguous(gate, "SwiGLU"); !s.ok())
        return s;
    if (auto s = check_contiguous(up, "SwiGLU"); !s.ok())
        return s;

    if (out.shape() != gate.shape() || out.shape() != up.shape()) {
        return Status::Error(ErrorCode::kInvalidArgument, "SwiGLU: shape mismatch");
    }
    if (out.dtype() != DType::kF32) {
        return Status::Error(ErrorCode::kUnsupported, "SwiGLU: output must be f32 (CPU debug)");
    }

    const int64_t n = out.shape().numel();
    auto* od = out.data_as<float>();

    for (int64_t i = 0; i < n; ++i) {
        const float g = elem_to_f32(gate.data(), gate.dtype(), i);
        const float u = elem_to_f32(up.data(), up.dtype(), i);
        // SiLU(g) * u = g * sigmoid(g) * u
        const float silu = g / (1.0f + std::exp(-g));
        od[i] = silu * u;
    }
    return {};
}

// AddInPlace

Status CpuBackend::AddInPlace(TensorView x, TensorView residual) {
    if (auto s = check_contiguous(x, "AddInPlace"); !s.ok())
        return s;
    if (auto s = check_contiguous(residual, "AddInPlace"); !s.ok())
        return s;

    if (x.shape() != residual.shape()) {
        return Status::Error(ErrorCode::kInvalidArgument, "AddInPlace: shape mismatch");
    }

    const int64_t n = x.shape().numel();

    if (x.dtype() == DType::kF32) {
        auto* xd = x.data_as<float>();
        for (int64_t i = 0; i < n; ++i) {
            xd[i] += elem_to_f32(residual.data(), residual.dtype(), i);
        }
    } else if (x.dtype() == DType::kF16) {
        auto* xd = x.data_as<uint16_t>();
        for (int64_t i = 0; i < n; ++i) {
            const float v = fp16_to_fp32(xd[i]) + elem_to_f32(residual.data(), residual.dtype(), i);
            xd[i] = fp32_to_fp16(v);
        }
    } else {
        return Status::Error(ErrorCode::kUnsupported, "AddInPlace: unsupported dtype");
    }
    return {};
}

// AddBiasInPlace

Status CpuBackend::AddBiasInPlace(TensorView x, TensorView bias) {
    if (auto s = check_contiguous(x, "AddBiasInPlace"); !s.ok())
        return s;
    if (auto s = check_contiguous(bias, "AddBiasInPlace"); !s.ok())
        return s;

    if (x.shape().rank() != 2 || bias.shape().numel() != x.shape().dim(1)) {
        return Status::Error(ErrorCode::kInvalidArgument,
                             "AddBiasInPlace: expected x[batch, n] + bias[n]");
    }

    const int64_t batch = x.shape().dim(0);
    const int64_t n = x.shape().dim(1);

    if (x.dtype() == DType::kF32) {
        auto* xd = x.data_as<float>();
        for (int64_t b = 0; b < batch; ++b) {
            for (int64_t i = 0; i < n; ++i) {
                xd[b * n + i] += elem_to_f32(bias.data(), bias.dtype(), i);
            }
        }
    } else if (x.dtype() == DType::kF16) {
        auto* xd = x.data_as<uint16_t>();
        for (int64_t b = 0; b < batch; ++b) {
            for (int64_t i = 0; i < n; ++i) {
                const float v =
                    fp16_to_fp32(xd[b * n + i]) + elem_to_f32(bias.data(), bias.dtype(), i);
                xd[b * n + i] = fp32_to_fp16(v);
            }
        }
    } else {
        return Status::Error(ErrorCode::kUnsupported, "AddBiasInPlace: unsupported dtype");
    }
    return {};
}

// LayerNorm

Status CpuBackend::LayerNorm(
    TensorView out, TensorView x, TensorView weight, TensorView bias, float eps) {
    if (auto s = check_contiguous(out, "LayerNorm"); !s.ok())
        return s;
    if (auto s = check_contiguous(x, "LayerNorm"); !s.ok())
        return s;
    if (auto s = check_contiguous(weight, "LayerNorm"); !s.ok())
        return s;
    if (bias.valid()) {
        if (auto s = check_contiguous(bias, "LayerNorm"); !s.ok())
            return s;
    }

    if (x.shape().rank() != 2 || out.shape() != x.shape() ||
        weight.shape().numel() != x.shape().dim(1) ||
        (bias.valid() && bias.shape().numel() != x.shape().dim(1))) {
        return Status::Error(ErrorCode::kInvalidArgument, "LayerNorm: shape mismatch");
    }
    if (!(eps > 0.0f)) {
        return Status::Error(ErrorCode::kInvalidArgument, "LayerNorm: eps must be positive");
    }
    if (out.dtype() != DType::kF32) {
        return Status::Error(ErrorCode::kUnsupported, "LayerNorm: output must be f32 (CPU debug)");
    }

    const int32_t batch = static_cast<int32_t>(x.shape().dim(0));
    const int32_t hidden = static_cast<int32_t>(x.shape().dim(1));
    auto* od = out.data_as<float>();
    const float inv = 1.0f / static_cast<float>(hidden);

    for (int32_t b = 0; b < batch; ++b) {
        const int64_t base = static_cast<int64_t>(b) * hidden;
        float mean = 0.0f;
        for (int32_t i = 0; i < hidden; ++i) {
            mean += elem_to_f32(x.data(), x.dtype(), base + i);
        }
        mean *= inv;
        float var = 0.0f;
        for (int32_t i = 0; i < hidden; ++i) {
            const float d = elem_to_f32(x.data(), x.dtype(), base + i) - mean;
            var += d * d;
        }
        var *= inv;
        const float denom = 1.0f / std::sqrt(var + eps);
        for (int32_t i = 0; i < hidden; ++i) {
            const float xv = elem_to_f32(x.data(), x.dtype(), base + i);
            const float wv = elem_to_f32(weight.data(), weight.dtype(), i);
            const float bv = bias.valid() ? elem_to_f32(bias.data(), bias.dtype(), i) : 0.0f;
            od[static_cast<size_t>(base) + static_cast<size_t>(i)] = (xv - mean) * denom * wv + bv;
        }
    }
    return {};
}

// GELU

Status CpuBackend::GeluInPlace(TensorView x, bool tanh_approx) {
    if (auto s = check_contiguous(x, "GeluInPlace"); !s.ok())
        return s;
    if (x.dtype() != DType::kF32) {
        return Status::Error(ErrorCode::kUnsupported, "GeluInPlace: f32 required (CPU reference)");
    }

    constexpr float kSqrt2OverPi = 0.7978845608028654f; // sqrt(2/pi)
    auto* xd = x.data_as<float>();
    const int64_t n = x.shape().numel();
    for (int64_t i = 0; i < n; ++i) {
        const float v = xd[i];
        if (tanh_approx) {
            const float inner = kSqrt2OverPi * (v + 0.044715f * v * v * v);
            xd[i] = 0.5f * v * (1.0f + std::tanh(inner));
        } else {
            xd[i] = 0.5f * v * (1.0f + std::erf(v * 0.7071067811865476f)); // 1/sqrt(2)
        }
    }
    return {};
}

// AttentionFull (vision: bidirectional, no mask, no KV cache)

Status CpuBackend::AttentionFull(
    TensorView out, TensorView q, TensorView k, TensorView v, const AttentionConfig& config) {
    if (auto s = check_contiguous(out, "AttentionFull"); !s.ok())
        return s;
    if (auto s = check_contiguous(q, "AttentionFull"); !s.ok())
        return s;
    if (auto s = check_contiguous(k, "AttentionFull"); !s.ok())
        return s;
    if (auto s = check_contiguous(v, "AttentionFull"); !s.ok())
        return s;

    if (q.shape().rank() != 3 || k.shape() != q.shape() || v.shape() != q.shape()) {
        return Status::Error(ErrorCode::kInvalidArgument,
                             "AttentionFull: q/k/v must be [n, heads, head_dim] with equal shapes");
    }
    const int32_t n = static_cast<int32_t>(q.shape().dim(0));
    const int32_t num_heads = static_cast<int32_t>(q.shape().dim(1));
    const int32_t head_dim = static_cast<int32_t>(q.shape().dim(2));
    if (config.num_heads != num_heads || config.head_dim != head_dim ||
        config.num_kv_heads != num_heads) {
        return Status::Error(
            ErrorCode::kInvalidArgument,
            "AttentionFull: config mismatch (MHA requires num_kv_heads == num_heads)");
    }
    if (out.shape().rank() != 2 || out.shape().dim(0) != n ||
        out.shape().dim(1) != num_heads * head_dim) {
        return Status::Error(ErrorCode::kInvalidArgument, "AttentionFull: output shape mismatch");
    }
    if (out.dtype() != DType::kF32) {
        return Status::Error(ErrorCode::kUnsupported,
                             "AttentionFull: output must be f32 (CPU debug)");
    }

    const float scale =
        config.scale > 0.0f ? config.scale : 1.0f / std::sqrt(static_cast<float>(head_dim));
    auto* od = out.data_as<float>();

    if (q.dtype() == DType::kF32 && k.dtype() == DType::kF32 && v.dtype() == DType::kF32) {
        // f32 fast path (the vision tower and host-resident LM attention):
        // direct row pointers instead of per-element elem_to_f32 dispatch,
        // 8-way partial sums for the q·k reduction (breaks the FMA latency
        // chain), and an AXPY-shaped weighted-V accumulation that vectorizes
        // cleanly. Scores/accumulator buffers are per-task and reused.
        const float* qb = q.data_as<const float>();
        const float* kb = k.data_as<const float>();
        const float* vb = v.data_as<const float>();
        const size_t row_stride = static_cast<size_t>(num_heads) * static_cast<size_t>(head_dim);
        parallel_for_rows(num_heads * n, [&](int32_t fb, int32_t fe) {
            std::vector<float> scores(static_cast<size_t>(n));
            std::vector<float> acc(static_cast<size_t>(head_dim));
            for (int32_t f = fb; f < fe; ++f) {
                const int32_t h = f / n;
                const int32_t i = f - h * n;
                const float* q_row = qb + (static_cast<size_t>(i) * static_cast<size_t>(num_heads) +
                                           static_cast<size_t>(h)) *
                                              static_cast<size_t>(head_dim);
                float max_score = -std::numeric_limits<float>::infinity();
                for (int32_t j = 0; j < n; ++j) {
                    const float* k_row =
                        kb + (static_cast<size_t>(j) * static_cast<size_t>(num_heads) +
                              static_cast<size_t>(h)) *
                                 static_cast<size_t>(head_dim);
                    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
                    float s4 = 0.0f, s5 = 0.0f, s6 = 0.0f, s7 = 0.0f;
                    int32_t d = 0;
                    for (; d + 8 <= head_dim; d += 8) {
                        s0 += q_row[d] * k_row[d];
                        s1 += q_row[d + 1] * k_row[d + 1];
                        s2 += q_row[d + 2] * k_row[d + 2];
                        s3 += q_row[d + 3] * k_row[d + 3];
                        s4 += q_row[d + 4] * k_row[d + 4];
                        s5 += q_row[d + 5] * k_row[d + 5];
                        s6 += q_row[d + 6] * k_row[d + 6];
                        s7 += q_row[d + 7] * k_row[d + 7];
                    }
                    s0 += ((s1 + s2) + (s3 + s4)) + ((s5 + s6) + s7);
                    for (; d < head_dim; ++d) {
                        s0 += q_row[d] * k_row[d];
                    }
                    const float sc = s0 * scale;
                    scores[static_cast<size_t>(j)] = sc;
                    max_score = std::max(max_score, sc);
                }
                float sum_exp = 0.0f;
                for (int32_t j = 0; j < n; ++j) {
                    const float e = std::exp(scores[static_cast<size_t>(j)] - max_score);
                    scores[static_cast<size_t>(j)] = e;
                    sum_exp += e;
                }
                const float inv_sum = 1.0f / sum_exp;
                std::fill(acc.begin(), acc.end(), 0.0f);
                for (int32_t j = 0; j < n; ++j) {
                    const float w = scores[static_cast<size_t>(j)] * inv_sum;
                    const float* v_row =
                        vb + (static_cast<size_t>(j) * static_cast<size_t>(num_heads) +
                              static_cast<size_t>(h)) *
                                 static_cast<size_t>(head_dim);
                    for (int32_t d = 0; d < head_dim; ++d) {
                        acc[static_cast<size_t>(d)] += w * v_row[d];
                    }
                }
                float* o_row = od + (static_cast<size_t>(i) * row_stride +
                                     static_cast<size_t>(h) * static_cast<size_t>(head_dim));
                std::memcpy(o_row, acc.data(), static_cast<size_t>(head_dim) * sizeof(float));
            }
        });
        return {};
    }

    // (head, token) pairs are independent; flatten them into a single task
    // space and hand ranges to the worker threads. The scores buffer is
    // thread-local and reused across iterations.
    parallel_for_rows(num_heads * n, [&](int32_t fb, int32_t fe) {
        std::vector<float> scores(static_cast<size_t>(n));
        for (int32_t f = fb; f < fe; ++f) {
            const int32_t h = f / n;
            const int32_t i = f - h * n;
            // Scores over all tokens (bidirectional).
            float max_score = -std::numeric_limits<float>::infinity();
            for (int32_t j = 0; j < n; ++j) {
                float dot = 0.0f;
                for (int32_t d = 0; d < head_dim; ++d) {
                    const float qv =
                        elem_to_f32(q.data(),
                                    q.dtype(),
                                    (static_cast<int64_t>(i) * num_heads + h) * head_dim + d);
                    const float kv =
                        elem_to_f32(k.data(),
                                    k.dtype(),
                                    (static_cast<int64_t>(j) * num_heads + h) * head_dim + d);
                    dot += qv * kv;
                }
                scores[static_cast<size_t>(j)] = dot * scale;
                if (scores[static_cast<size_t>(j)] > max_score) {
                    max_score = scores[static_cast<size_t>(j)];
                }
            }
            float sum_exp = 0.0f;
            for (int32_t j = 0; j < n; ++j) {
                scores[static_cast<size_t>(j)] =
                    std::exp(scores[static_cast<size_t>(j)] - max_score);
                sum_exp += scores[static_cast<size_t>(j)];
            }
            const float inv_sum = 1.0f / sum_exp;
            for (int32_t d = 0; d < head_dim; ++d) {
                float acc = 0.0f;
                for (int32_t j = 0; j < n; ++j) {
                    const float vv =
                        elem_to_f32(v.data(),
                                    v.dtype(),
                                    (static_cast<int64_t>(j) * num_heads + h) * head_dim + d);
                    acc += scores[static_cast<size_t>(j)] * vv * inv_sum;
                }
                od[(static_cast<size_t>(i) * static_cast<size_t>(num_heads) +
                    static_cast<size_t>(h)) *
                       static_cast<size_t>(head_dim) +
                   static_cast<size_t>(d)] = acc;
            }
        }
    });
    return {};
}

// RopeApply (table-driven rotary; neox pairing: dim i pairs with i + hd/2)

Status CpuBackend::RopeApply(TensorView q, TensorView k, TensorView cos, TensorView sin) {
    if (auto s = check_contiguous(q, "RopeApply"); !s.ok())
        return s;
    if (auto s = check_contiguous(k, "RopeApply"); !s.ok())
        return s;
    if (auto s = check_contiguous(cos, "RopeApply"); !s.ok())
        return s;
    if (auto s = check_contiguous(sin, "RopeApply"); !s.ok())
        return s;

    if (q.shape().rank() != 3 || k.shape().rank() != 3) {
        return Status::Error(ErrorCode::kInvalidArgument,
                             "RopeApply: q/k must be [n, heads, head_dim]");
    }
    const int32_t n = static_cast<int32_t>(q.shape().dim(0));
    const int32_t q_heads = static_cast<int32_t>(q.shape().dim(1));
    const int32_t kv_heads = static_cast<int32_t>(k.shape().dim(1));
    const int32_t head_dim = static_cast<int32_t>(q.shape().dim(2));
    if (k.shape().dim(0) != n || k.shape().dim(2) != head_dim || head_dim % 2 != 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "RopeApply: shape mismatch");
    }
    if (cos.shape().numel() != static_cast<int64_t>(n) * head_dim ||
        sin.shape().numel() != static_cast<int64_t>(n) * head_dim || cos.dtype() != DType::kF32 ||
        sin.dtype() != DType::kF32) {
        return Status::Error(ErrorCode::kInvalidArgument,
                             "RopeApply: cos/sin must be f32 [n, head_dim]");
    }
    if (q.dtype() != DType::kF32 || k.dtype() != DType::kF32) {
        return Status::Error(ErrorCode::kUnsupported, "RopeApply: f32 required (CPU reference)");
    }

    const float* cd = cos.data_as<const float>();
    const float* sd = sin.data_as<const float>();
    const int32_t half = head_dim / 2;

    auto apply = [&](TensorView t, int32_t heads) {
        auto* d = t.data_as<float>();
        for (int32_t b = 0; b < n; ++b) {
            const float* crow = cd + static_cast<size_t>(b) * static_cast<size_t>(head_dim);
            const float* srow = sd + static_cast<size_t>(b) * static_cast<size_t>(head_dim);
            for (int32_t h = 0; h < heads; ++h) {
                float* row = d + (static_cast<size_t>(b) * static_cast<size_t>(heads) +
                                  static_cast<size_t>(h)) *
                                     static_cast<size_t>(head_dim);
                for (int32_t i = 0; i < half; ++i) {
                    const float x0 = row[i];
                    const float x1 = row[i + half];
                    row[i] = x0 * crow[i] - x1 * srow[i];
                    row[i + half] = x0 * srow[i] + x1 * crow[i];
                }
            }
        }
    };
    apply(q, q_heads);
    apply(k, kv_heads);
    return {};
}

} // namespace pl::mllm
