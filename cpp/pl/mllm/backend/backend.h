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

    // Fused GEMV: compute multiple MatMuls sharing the same input x in a
    // single kernel dispatch. Reduces kernel launch overhead by N-1x.
    // out[i]   shape: [batch, out_dims[i]]
    // x        shape: [batch, in_dim]
    // weights  shape: [out_dims[i], in_dim] each (imported, identified by name)
    // Default implementation falls back to N separate MatMul calls.
    virtual Status MatMulFused(std::span<TensorView> outs,
                               TensorView x,
                               std::span<const std::string_view> weight_names) {
        if (outs.size() != weight_names.size()) {
            return Status::Error(ErrorCode::kInvalidArgument,
                                 "MatMulFused: size mismatch");
        }
        for (size_t i = 0; i < outs.size(); ++i) {
            if (auto s = MatMul(outs[i], x, weight_names[i]); !s.ok()) {
                return s;
            }
        }
        return {};
    }

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

    // x[b, i] += bias[i]  (in-place, row-broadcast)
    // x shape: [batch, n], bias shape: [n]
    virtual Status AddBiasInPlace(TensorView x, TensorView bias) = 0;

    // Block until all queued work is complete.
    virtual Status Synchronize() = 0;

    // --- Optional device-residency hooks -----------------------------------
    // Backends that keep activations device-resident (e.g. Metal) implement
    // these; the default implementations preserve plain host-memory semantics
    // (every op reads/writes host tensors synchronously), so CpuBackend and
    // existing callers need no changes.

    // Called after the host writes into `t`'s memory outside of the backend
    // (e.g. the engine's embedding lookup). The backend must treat any cached
    // device-side copy of `t` as stale and re-upload before the next use.
    virtual Status NotifyHostWrite(TensorView t) {
        (void)t;
        return {};
    }

    // Ensures `t`'s host memory reflects the latest backend-computed results.
    // Must be called before the host reads a backend-produced tensor (e.g.
    // logits before sampling). Backends with deferred execution flush and
    // synchronize here.
    virtual Status SyncToHost(TensorView t) {
        (void)t;
        return {};
    }

    // --- Optional device-resident KV cache ---------------------------------
    // When HasDeviceKV() returns true, the model appends K/V and runs
    // attention through the backend (AppendKV/AttentionKV) instead of the
    // host KVCache; the host cache is then only used for length/capacity
    // bookkeeping. All three must be implemented together.

    virtual bool HasDeviceKV() const { return false; }

    // Allocate device-side K/V storage: [num_layers][capacity][nkv*head_dim]
    // per cache, f32 elements.
    virtual Status ConfigureDeviceKV(int32_t num_layers,
                                     int32_t num_kv_heads,
                                     int32_t head_dim,
                                     int32_t capacity) {
        (void)num_layers;
        (void)num_kv_heads;
        (void)head_dim;
        (void)capacity;
        return Status::Error(ErrorCode::kUnsupported, "device KV cache not supported");
    }

    // Append one token's K/V for `layer` at absolute `position`.
    // key/value shape: [1, num_kv_heads, head_dim]
    virtual Status AppendKV(int32_t layer, TensorView key, TensorView value, int64_t position) {
        (void)layer;
        (void)key;
        (void)value;
        (void)position;
        return Status::Error(ErrorCode::kUnsupported, "device KV cache not supported");
    }

    // Single-token causal attention over the device KV cache of `layer`,
    // attending to positions [0, seq_len).
    // q shape: [1, num_heads, head_dim]; out shape: [1, num_heads * head_dim]
    virtual Status AttentionKV(TensorView out,
                               TensorView q,
                               int32_t layer,
                               int64_t seq_len,
                               const AttentionConfig& config) {
        (void)out;
        (void)q;
        (void)layer;
        (void)seq_len;
        (void)config;
        return Status::Error(ErrorCode::kUnsupported, "device KV cache not supported");
    }
};

} // namespace pl::mllm
