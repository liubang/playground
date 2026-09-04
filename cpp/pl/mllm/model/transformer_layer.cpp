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

#include "cpp/pl/mllm/model/transformer_layer.h"

#include <array>
#include <cmath>

namespace pl::mllm {

Status TransformerLayer::Forward(TensorView hidden,
                                 int64_t position,
                                 KVCache& cache,
                                 Backend& backend,
                                 ScratchArena& scratch,
                                 const ModelConfig& config) const {
    const int32_t hidden_size = config.hidden_size;
    const int32_t num_heads = config.num_attention_heads;
    const int32_t num_kv_heads = config.num_kv_heads;
    const int32_t head_dim = config.effective_head_dim();
    const float scale = 1.0f / std::sqrt(static_cast<float>(head_dim));

    // 1. Attention RMSNorm
    auto norm_out = scratch.AllocateTensor({1, hidden_size}, DType::kF32);
    if (!norm_out.ok())
        return norm_out.status();
    auto attn_out = norm_out.value();

    if (auto s = backend.RmsNorm(attn_out, hidden, weights_.attn_norm, config.rms_norm_eps);
        !s.ok()) {
        return s;
    }

    // 2. Q/K/V projections (fused into a single GEMV dispatch)
    // Q: [1, num_heads * head_dim]
    // K: [1, num_kv_heads * head_dim]
    // V: [1, num_kv_heads * head_dim]
    auto q_out = scratch.AllocateTensor({1, num_heads * head_dim}, DType::kF32);
    if (!q_out.ok())
        return q_out.status();
    auto k_out = scratch.AllocateTensor({1, num_kv_heads * head_dim}, DType::kF32);
    if (!k_out.ok())
        return k_out.status();
    auto v_out = scratch.AllocateTensor({1, num_kv_heads * head_dim}, DType::kF32);
    if (!v_out.ok())
        return v_out.status();

    auto q = q_out.value();
    auto k = k_out.value();
    auto v = v_out.value();

    {
        std::array<TensorView, 3> fused_outs = {q, k, v};
        std::array<std::string_view, 3> fused_names = {
            weights_.q_weight_name, weights_.k_weight_name, weights_.v_weight_name};
        if (auto s = backend.MatMulFused(fused_outs, attn_out, fused_names); !s.ok())
            return s;
    }

    // 3a. Optional Q/K/V bias (Qwen2): row-broadcast add after projection.
    if (weights_.q_bias.valid()) {
        if (auto s = backend.AddBiasInPlace(q, weights_.q_bias); !s.ok())
            return s;
    }
    if (weights_.k_bias.valid()) {
        if (auto s = backend.AddBiasInPlace(k, weights_.k_bias); !s.ok())
            return s;
    }
    if (weights_.v_bias.valid()) {
        if (auto s = backend.AddBiasInPlace(v, weights_.v_bias); !s.ok())
            return s;
    }

    // 3b + 4. Optional per-head Q/K RMSNorm (Qwen3) FUSED into the RoPE call:
    // backends apply the norm before rotation inside one kernel (Metal) or
    // inline (CPU reference). Saves 2 kernel dispatches per layer on device
    // backends.
    auto q_reshaped = q.reshape({1, num_heads, head_dim});
    if (!q_reshaped.ok())
        return q_reshaped.status();
    auto k_reshaped = k.reshape({1, num_kv_heads, head_dim});
    if (!k_reshaped.ok())
        return k_reshaped.status();

    RopeConfig rope_cfg{
        .head_dim = head_dim,
        .freq_base = config.rope_freq_base,
    };
    if (weights_.q_norm.valid()) {
        rope_cfg.q_norm = weights_.q_norm;
        rope_cfg.k_norm = weights_.k_norm;
        rope_cfg.rms_eps = config.rms_norm_eps;
    }
    if (auto s = backend.RoPE(q_reshaped.value(), k_reshaped.value(), position, rope_cfg);
        !s.ok()) {
        return s;
    }

    // 5/6. Append K/V and run attention. Backends with device-resident KV
    // storage (HasDeviceKV) take over both steps; otherwise use the host
    // KV cache and the generic Attention op.
    auto v_reshaped = v.reshape({1, num_kv_heads, head_dim});
    if (!v_reshaped.ok())
        return v_reshaped.status();

    auto attn_ctx = scratch.AllocateTensor({1, num_heads * head_dim}, DType::kF32);
    if (!attn_ctx.ok())
        return attn_ctx.status();
    auto attn_ctx_out = attn_ctx.value();

    AttentionConfig attn_cfg{
        .num_heads = num_heads,
        .num_kv_heads = num_kv_heads,
        .head_dim = head_dim,
        .scale = scale,
    };
    if (backend.HasDeviceKV()) {
        // Device KV buffers are addressed physically: slot = absolute
        // position - window origin (0 in strict mode, >0 after ring drops).
        const int64_t phys_pos = position - cache.window_origin();
        if (auto s =
                backend.AppendKV(layer_index_, k_reshaped.value(), v_reshaped.value(), phys_pos);
            !s.ok()) {
            return s;
        }
        // The query attends to positions [0, position] inclusive.
        if (auto s = backend.AttentionKV(
                attn_ctx_out, q_reshaped.value(), layer_index_, phys_pos + 1, attn_cfg);
            !s.ok()) {
            return s;
        }
    } else {
        if (auto s = cache.Append(layer_index_, k_reshaped.value(), v_reshaped.value()); !s.ok()) {
            return s;
        }
        KVCacheView kv_view = cache.View(layer_index_);
        // View() only reports fully-advanced tokens. This token was appended
        // above but not yet advanced, so extend the view by one so the query
        // can attend to the causal diagonal (itself and all previous
        // positions).
        ++kv_view.seq_len;
        if (auto s = backend.Attention(attn_ctx_out, q_reshaped.value(), kv_view, attn_cfg);
            !s.ok()) {
            return s;
        }
    }

    // 7. Output projection
    auto proj_out = scratch.AllocateTensor({1, hidden_size}, DType::kF32);
    if (!proj_out.ok())
        return proj_out.status();
    auto proj = proj_out.value();

    if (auto s = backend.MatMul(proj, attn_ctx_out, weights_.o_weight_name); !s.ok()) {
        return s;
    }

    // 8 + 9. Fused residual add + MLP RMSNorm (one dispatch on device
    // backends; the default implementation composes add then norm).
    auto mlp_norm_out = scratch.AllocateTensor({1, hidden_size}, DType::kF32);
    if (!mlp_norm_out.ok())
        return mlp_norm_out.status();
    auto mlp_out = mlp_norm_out.value();

    if (auto s = backend.RmsNormAdd(mlp_out, hidden, proj, weights_.mlp_norm, config.rms_norm_eps);
        !s.ok()) {
        return s;
    }

    // 10. Gate / Up projections (fused into a single GEMV dispatch)
    const int32_t inter = config.intermediate_size;
    auto gate_out = scratch.AllocateTensor({1, inter}, DType::kF32);
    if (!gate_out.ok())
        return gate_out.status();
    auto up_out = scratch.AllocateTensor({1, inter}, DType::kF32);
    if (!up_out.ok())
        return up_out.status();
    auto gate = gate_out.value();
    auto up = up_out.value();

    {
        std::array<TensorView, 2> fused_outs = {gate, up};
        std::array<std::string_view, 2> fused_names = {weights_.gate_weight_name,
                                                       weights_.up_weight_name};
        if (auto s = backend.MatMulFused(fused_outs, mlp_out, fused_names); !s.ok())
            return s;
    }

    // 11. SwiGLU
    auto act_out = scratch.AllocateTensor({1, inter}, DType::kF32);
    if (!act_out.ok())
        return act_out.status();
    auto act = act_out.value();

    if (auto s = backend.SwiGLU(act, gate, up); !s.ok())
        return s;

    // 12. Down projection
    auto down_out = scratch.AllocateTensor({1, hidden_size}, DType::kF32);
    if (!down_out.ok())
        return down_out.status();
    auto down = down_out.value();

    if (auto s = backend.MatMul(down, act, weights_.down_weight_name); !s.ok())
        return s;

    // 13. Residual add
    if (auto s = backend.AddInPlace(hidden, down); !s.ok())
        return s;

    return {};
}

namespace {

// Row slice of a contiguous 2D/3D tensor: row i of a [n, ...] tensor as a
// [1, ...] view sharing the storage.
TensorView row_slice(TensorView t, int32_t i) {
    const auto src = t.shape().dims();
    std::array<int64_t, Shape::kMaxRank> dims{};
    dims[0] = 1;
    const int rank = static_cast<int>(src.size());
    for (int j = 1; j < rank; ++j) {
        dims[j] = src[static_cast<size_t>(j)];
    }
    const int64_t row_elems = t.shape().numel() / t.shape().dim(0);
    return TensorView(static_cast<char*>(t.data()) +
                          static_cast<size_t>(i) * dtype_nbytes(t.dtype(), row_elems),
                      t.dtype(),
                      Shape(std::span<const int64_t>(dims.data(), static_cast<size_t>(rank))));
}

} // namespace

Status TransformerLayer::ForwardBatch(TensorView hidden,
                                      int64_t start_pos,
                                      KVCache& cache,
                                      Backend& backend,
                                      ScratchArena& scratch,
                                      const ModelConfig& config) const {
    const int32_t n = static_cast<int32_t>(hidden.shape().dim(0));
    const int32_t hidden_size = config.hidden_size;
    const int32_t num_heads = config.num_attention_heads;
    const int32_t num_kv_heads = config.num_kv_heads;
    const int32_t head_dim = config.effective_head_dim();
    const float scale = 1.0f / std::sqrt(static_cast<float>(head_dim));

    if (hidden.shape().rank() != 2 || hidden.shape().dim(1) != hidden_size) {
        return Status::Error(ErrorCode::kInvalidArgument,
                             "ForwardBatch: hidden must be [n, hidden_size]");
    }
    if (hidden.dtype() != DType::kF32) {
        return Status::Error(ErrorCode::kInvalidArgument, "ForwardBatch: hidden must be f32");
    }

    // 1. Attention RMSNorm [n, hidden]
    auto norm_out = scratch.AllocateTensor({n, hidden_size}, DType::kF32);
    if (!norm_out.ok())
        return norm_out.status();
    auto attn_out = norm_out.value();

    if (auto s = backend.RmsNorm(attn_out, hidden, weights_.attn_norm, config.rms_norm_eps);
        !s.ok()) {
        return s;
    }

    // 2. Q/K/V projections — MatMulFused falls back to per-matrix batches
    //    when the backend's fused kernel only supports GEMV.
    auto q_out = scratch.AllocateTensor({n, num_heads * head_dim}, DType::kF32);
    if (!q_out.ok())
        return q_out.status();
    auto k_out = scratch.AllocateTensor({n, num_kv_heads * head_dim}, DType::kF32);
    if (!k_out.ok())
        return k_out.status();
    auto v_out = scratch.AllocateTensor({n, num_kv_heads * head_dim}, DType::kF32);
    if (!v_out.ok())
        return v_out.status();
    auto q = q_out.value();
    auto k = k_out.value();
    auto v = v_out.value();

    {
        std::array<TensorView, 3> fused_outs = {q, k, v};
        std::array<std::string_view, 3> fused_names = {
            weights_.q_weight_name, weights_.k_weight_name, weights_.v_weight_name};
        if (auto s = backend.MatMulFused(fused_outs, attn_out, fused_names); !s.ok())
            return s;
    }

    // 3a. Optional Q/K/V bias (Qwen2)
    if (weights_.q_bias.valid()) {
        if (auto s = backend.AddBiasInPlace(q, weights_.q_bias); !s.ok())
            return s;
    }
    if (weights_.k_bias.valid()) {
        if (auto s = backend.AddBiasInPlace(k, weights_.k_bias); !s.ok())
            return s;
    }
    if (weights_.v_bias.valid()) {
        if (auto s = backend.AddBiasInPlace(v, weights_.v_bias); !s.ok())
            return s;
    }

    // 3b. Optional per-head Q/K RMSNorm (Qwen3): [n, heads*hd] ->
    // [n*heads, hd] so every (token, head) row is normalized independently.
    if (weights_.q_norm.valid()) {
        auto q_heads = q.reshape({n * num_heads, head_dim});
        if (!q_heads.ok())
            return q_heads.status();
        if (auto s = backend.RmsNorm(
                q_heads.value(), q_heads.value(), weights_.q_norm, config.rms_norm_eps);
            !s.ok()) {
            return s;
        }
    }
    if (weights_.k_norm.valid()) {
        auto k_heads = k.reshape({n * num_kv_heads, head_dim});
        if (!k_heads.ok())
            return k_heads.status();
        if (auto s = backend.RmsNorm(
                k_heads.value(), k_heads.value(), weights_.k_norm, config.rms_norm_eps);
            !s.ok()) {
            return s;
        }
    }

    // 4. RoPE with per-row positions (row b -> start_pos + b).
    auto q_reshaped = q.reshape({n, num_heads, head_dim});
    if (!q_reshaped.ok())
        return q_reshaped.status();
    auto k_reshaped = k.reshape({n, num_kv_heads, head_dim});
    if (!k_reshaped.ok())
        return k_reshaped.status();
    auto v_reshaped = v.reshape({n, num_kv_heads, head_dim});
    if (!v_reshaped.ok())
        return v_reshaped.status();

    RopeConfig rope_cfg{
        .head_dim = head_dim,
        .freq_base = config.rope_freq_base,
    };
    if (auto s = backend.RoPE(q_reshaped.value(), k_reshaped.value(), start_pos, rope_cfg);
        !s.ok()) {
        return s;
    }

    // 5/6. Append K/V and run causal attention. The GEMMs above are batched;
    // attention runs per query row (each row attends to exactly the prefix
    // [0, start_pos + i], which is exactly causal masking).
    auto ctx_buf = scratch.AllocateTensor({n, num_heads * head_dim}, DType::kF32);
    if (!ctx_buf.ok())
        return ctx_buf.status();
    auto attn_ctx = ctx_buf.value();

    AttentionConfig attn_cfg{
        .num_heads = num_heads,
        .num_kv_heads = num_kv_heads,
        .head_dim = head_dim,
        .scale = scale,
    };
    if (backend.HasDeviceKV()) {
        // Whole-batch device-KV interaction: never pass row slices of a
        // backend-produced tensor to the backend — device-side shadow
        // buffers are keyed by base pointer, so a row slice would be
        // re-uploaded from stale host memory. AppendKV/AttentionPrefillKV
        // take the whole [n, ...] tensor at once.
        // Device KV is addressed physically (absolute - window origin).
        const int64_t origin = cache.window_origin();
        if (auto s = backend.AppendKV(
                layer_index_, k_reshaped.value(), v_reshaped.value(), start_pos - origin);
            !s.ok()) {
            return s;
        }
        if (auto s = backend.AttentionPrefillKV(
                attn_ctx, q_reshaped.value(), layer_index_, start_pos + 1 - origin, attn_cfg);
            !s.ok()) {
            return s;
        }
    } else {
        if (auto s = cache.AppendBatch(layer_index_, k_reshaped.value(), v_reshaped.value());
            !s.ok()) {
            return s;
        }
        // Read the origin AFTER AppendBatch: in ring mode the append may
        // have compacted (dropped the oldest tokens), advancing the origin.
        KVCacheView kv_base = cache.View(layer_index_);
        const int64_t origin = cache.window_origin();
        // Query row i sits at physical slot start_pos + i - origin and sees
        // every entry up to and including itself (strictly causal).
        for (int32_t i = 0; i < n; ++i) {
            KVCacheView kv = kv_base;
            kv.seq_len = static_cast<int32_t>(start_pos + i + 1 - origin);
            if (auto s = backend.Attention(
                    row_slice(attn_ctx, i), row_slice(q_reshaped.value(), i), kv, attn_cfg);
                !s.ok()) {
                return s;
            }
        }
    }

    // 7. Output projection
    auto proj_out = scratch.AllocateTensor({n, hidden_size}, DType::kF32);
    if (!proj_out.ok())
        return proj_out.status();
    auto proj = proj_out.value();

    if (auto s = backend.MatMul(proj, attn_ctx, weights_.o_weight_name); !s.ok()) {
        return s;
    }

    // 8. Residual add
    if (auto s = backend.AddInPlace(hidden, proj); !s.ok())
        return s;

    // 9. MLP RMSNorm
    auto mlp_norm_out = scratch.AllocateTensor({n, hidden_size}, DType::kF32);
    if (!mlp_norm_out.ok())
        return mlp_norm_out.status();
    auto mlp_out = mlp_norm_out.value();

    if (auto s = backend.RmsNorm(mlp_out, hidden, weights_.mlp_norm, config.rms_norm_eps);
        !s.ok()) {
        return s;
    }

    // 10. Gate / Up projections
    const int32_t inter = config.intermediate_size;
    auto gate_out = scratch.AllocateTensor({n, inter}, DType::kF32);
    if (!gate_out.ok())
        return gate_out.status();
    auto up_out = scratch.AllocateTensor({n, inter}, DType::kF32);
    if (!up_out.ok())
        return up_out.status();
    auto gate = gate_out.value();
    auto up = up_out.value();

    {
        std::array<TensorView, 2> fused_outs = {gate, up};
        std::array<std::string_view, 2> fused_names = {weights_.gate_weight_name,
                                                       weights_.up_weight_name};
        if (auto s = backend.MatMulFused(fused_outs, mlp_out, fused_names); !s.ok())
            return s;
    }

    // 11. SwiGLU
    auto act_out = scratch.AllocateTensor({n, inter}, DType::kF32);
    if (!act_out.ok())
        return act_out.status();
    auto act = act_out.value();

    if (auto s = backend.SwiGLU(act, gate, up); !s.ok())
        return s;

    // 12. Down projection
    auto down_out = scratch.AllocateTensor({n, hidden_size}, DType::kF32);
    if (!down_out.ok())
        return down_out.status();
    auto down = down_out.value();

    if (auto s = backend.MatMul(down, act, weights_.down_weight_name); !s.ok()) {
        return s;
    }

    // 13. Residual add
    if (auto s = backend.AddInPlace(hidden, down); !s.ok())
        return s;

    return {};
}

} // namespace pl::mllm
