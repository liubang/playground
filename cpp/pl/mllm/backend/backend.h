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

#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "cpp/pl/mllm/core/status.h"
#include "cpp/pl/mllm/core/tensor.h"

namespace pl::mllm {

// Weight reference passed to backends. The name maps to a tensor that was
// imported via ImportWeights; the backend owns the actual device buffer.
struct WeightRef {
    std::string_view name;
};

// RoPE configuration matching LLaMA conventions.
struct RopeConfig {
    int32_t head_dim = 0;
    float freq_base = 10000.0f;
    // If >0, scale low-frequency dims (NTK-aware, NTK-by-parts, etc.).
    float freq_scale = 1.0f;
};

// Immutable view of one KV cache slot for a single layer.
struct KVCacheView {
    const void* keys = nullptr; // [seq_len, num_kv_heads, head_dim]
    const void* values = nullptr;
    int32_t seq_len = 0; // valid entries
    int32_t num_kv_heads = 0;
    int32_t head_dim = 0;
    DType dtype = DType::kF16;
};

// Attention configuration for decode / prefill.
struct AttentionConfig {
    int32_t num_heads = 0;    // query heads
    int32_t num_kv_heads = 0; // GQA group size = num_heads / num_kv_heads
    int32_t head_dim = 0;
    float scale = 0.0f; // 1/sqrt(head_dim), precomputed by caller
};

// Pluggable compute backend. The MVP uses virtual dispatch; see SPEC §5.1.
class Backend {
public:
    virtual ~Backend() = default;

    // Import (or reference) model weights identified by name.
    // The backend must retain valid device-side storage for the full model lifetime.
    virtual Status ImportWeights(std::span<const TensorView> weights,
                                 std::span<const std::string_view> names) = 0;

    // out  = x * weight^T  (GEMV/GEMM, single-token decode or small-batch prefill)
    // out shape: [1, out_dim]  (decode) or [batch, out_dim] (prefill)
    // x    shape: [batch, in_dim]
    // weight (imported) shape: [out_dim, in_dim]
    virtual Status MatMul(TensorView out, TensorView x, std::string_view weight_name) = 0;

    // RMSNorm: out = x / sqrt(mean(x^2) + eps) * weight
    // x, out, weight shape: [batch, hidden] (or [1, hidden])
    virtual Status RmsNorm(TensorView out, TensorView x, TensorView weight, float eps) = 0;

    // In-place RoPE for query/key tensors.
    // q shape: [batch, num_heads, head_dim]
    // k shape: [batch, num_kv_heads, head_dim]
    virtual Status RoPE(TensorView q, TensorView k, int64_t position, const RopeConfig& config) = 0;

    // Causal multi-head attention with GQA.
    // q shape: [1, num_heads, head_dim]
    // out shape: [1, num_heads * head_dim]
    virtual Status Attention(TensorView out,
                             TensorView q,
                             const KVCacheView& kv,
                             const AttentionConfig& config) = 0;

    // SwiGLU activation: out = silu(gate) * up
    // All tensors shape: [batch, intermediate]
    virtual Status SwiGLU(TensorView out, TensorView gate, TensorView up) = 0;

    // x += residual  (in-place)
    virtual Status AddInPlace(TensorView x, TensorView residual) = 0;

    // Block until all queued work is complete.
    virtual Status Synchronize() = 0;
};

} // namespace pl::mllm
