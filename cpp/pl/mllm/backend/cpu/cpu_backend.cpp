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

#include <cmath>
#include <cstring>
#include <limits>
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
        case DType::kQ8_0: {
            const auto* blocks = static_cast<const Q8Block*>(base);
            const int64_t block_idx = idx / kQ8_0BlockSize;
            const int64_t in_block = idx % kQ8_0BlockSize;
            const float scale = fp16_to_fp32(blocks[block_idx].scale);
            return scale * static_cast<float>(blocks[block_idx].qs[in_block]);
        }
        default:
            return 0.0f; // Q4_0 not yet in CPU backend
    }
}

// MatMul kernels

// out[b, o] = sum_i x[b, i] * w[o, i]   (w stored as [out_dim, in_dim])
// XT/WT are the element types of x/weight; both convert to float for the MAC.
template <typename XT, typename WT>
void matmul_fxx(
    float* out, const XT* x, const WT* w, int32_t batch, int32_t out_dim, int32_t in_dim) {
    for (int32_t b = 0; b < batch; ++b) {
        const XT* xb = x + static_cast<size_t>(b) * static_cast<size_t>(in_dim);
        float* ob = out + static_cast<size_t>(b) * static_cast<size_t>(out_dim);
        for (int32_t o = 0; o < out_dim; ++o) {
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
template <typename XT>
void matmul_q8_0(
    float* out, const XT* x, const Q8Block* w, int32_t batch, int32_t out_dim, int32_t in_dim) {
    constexpr int64_t block = kQ8_0BlockSize;
    const int32_t num_blocks = in_dim / static_cast<int32_t>(block);

    for (int32_t b = 0; b < batch; ++b) {
        const XT* xb = x + static_cast<size_t>(b) * static_cast<size_t>(in_dim);
        float* ob = out + static_cast<size_t>(b) * static_cast<size_t>(out_dim);
        for (int32_t o = 0; o < out_dim; ++o) {
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

// RoPE helpers

// Compute theta_i for dim i: 1 / (freq_base ^ (2i/head_dim))
inline float rope_theta(int32_t i, int32_t head_dim, float freq_base) {
    return 1.0f / std::pow(freq_base, static_cast<float>(2 * i) / static_cast<float>(head_dim));
}

// Apply RoPE in-place to a single head of `head_dim` elements.
// LLaMA uses "GPT-NeoX style" interleaved rotation:
//   x'[2i]   = x[2i] * cos(p*theta_i) - x[2i+1] * sin(p*theta_i)
//   x'[2i+1] = x[2i] * sin(p*theta_i) + x[2i+1] * cos(p*theta_i)
template <typename T>
void apply_rope_head(T* ptr, int32_t head_dim, int64_t position, float freq_base) {
    const float p = static_cast<float>(position);
    for (int32_t i = 0; i < head_dim / 2; ++i) {
        const float theta = rope_theta(i, head_dim, freq_base);
        const float angle = p * theta;
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        const float a = static_cast<float>(ptr[2 * i]);
        const float b = static_cast<float>(ptr[2 * i + 1]);
        ptr[2 * i] = static_cast<T>(a * c - b * s);
        ptr[2 * i + 1] = static_cast<T>(a * s + b * c);
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
        T* qb = qd + static_cast<size_t>(b) * static_cast<size_t>(q_stride);
        T* kb = kd + static_cast<size_t>(b) * static_cast<size_t>(k_stride);
        for (int32_t h = 0; h < num_heads; ++h) {
            apply_rope_head(qb + h * head_dim, head_dim, position, freq_base);
        }
        for (int32_t h = 0; h < num_kv_heads; ++h) {
            apply_rope_head(kb + h * head_dim, head_dim, position, freq_base);
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
    auto it = weights_.find(std::string(name));
    return it != weights_.end() ? &it->second : nullptr;
}

// MatMul

Status CpuBackend::MatMul(TensorView out, TensorView x, std::string_view weight_name) {
    if (auto s = check_contiguous(out, "MatMul"); !s.ok())
        return s;
    if (auto s = check_contiguous(x, "MatMul"); !s.ok())
        return s;

    auto it = weights_.find(std::string(weight_name));
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

    if (w.dtype() == DType::kQ8_0) {
        if (in_dim % kQ8_0BlockSize != 0) {
            return Status::Error(ErrorCode::kInvalidArgument,
                                 "MatMul: in_dim not Q8_0 block-aligned");
        }
        const auto* wq = static_cast<const Q8Block*>(w.data());
        if (x.dtype() == DType::kF32) {
            matmul_q8_0(out_ptr, x.data_as<float>(), wq, batch, out_dim, in_dim);
        } else if (x.dtype() == DType::kF16) {
            matmul_q8_0(out_ptr, x.data_as<uint16_t>(), wq, batch, out_dim, in_dim);
        } else {
            return Status::Error(ErrorCode::kUnsupported,
                                 "MatMul: Q8_0 weight requires f32/f16 input");
        }
    } else if (w.dtype() == DType::kF32) {
        if (x.dtype() == DType::kF32) {
            matmul_fxx<float, float>(
                out_ptr, x.data_as<float>(), w.data_as<float>(), batch, out_dim, in_dim);
        } else if (x.dtype() == DType::kF16) {
            matmul_fxx<uint16_t, float>(
                out_ptr, x.data_as<uint16_t>(), w.data_as<float>(), batch, out_dim, in_dim);
        } else {
            return Status::Error(ErrorCode::kUnsupported, "MatMul: unsupported input dtype");
        }
    } else if (w.dtype() == DType::kF16) {
        if (x.dtype() == DType::kF32) {
            matmul_fxx<float, uint16_t>(
                out_ptr, x.data_as<float>(), w.data_as<uint16_t>(), batch, out_dim, in_dim);
        } else if (x.dtype() == DType::kF16) {
            matmul_fxx<uint16_t, uint16_t>(
                out_ptr, x.data_as<uint16_t>(), w.data_as<uint16_t>(), batch, out_dim, in_dim);
        } else {
            return Status::Error(ErrorCode::kUnsupported, "MatMul: unsupported input dtype");
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

} // namespace pl::mllm
