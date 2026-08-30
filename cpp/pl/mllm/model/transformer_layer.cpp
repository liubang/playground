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

    // 2. Q/K/V projections
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

    if (auto s = backend.MatMul(q, attn_out, weights_.q_weight_name); !s.ok())
        return s;
    if (auto s = backend.MatMul(k, attn_out, weights_.k_weight_name); !s.ok())
        return s;
    if (auto s = backend.MatMul(v, attn_out, weights_.v_weight_name); !s.ok())
        return s;

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

    // 3b. Optional per-head Q/K RMSNorm (Qwen3), applied before RoPE.
    //   q [1, heads*head_dim] -> [heads, head_dim] where each row is a head;
    //   the norm weight is [head_dim]. In-place (out aliases x) is safe for
    //   row-wise RMSNorm.
    if (weights_.q_norm.valid()) {
        auto q_heads = q.reshape({num_heads, head_dim});
        if (!q_heads.ok())
            return q_heads.status();
        if (auto s = backend.RmsNorm(
                q_heads.value(), q_heads.value(), weights_.q_norm, config.rms_norm_eps);
            !s.ok()) {
            return s;
        }
    }
    if (weights_.k_norm.valid()) {
        auto k_heads = k.reshape({num_kv_heads, head_dim});
        if (!k_heads.ok())
            return k_heads.status();
        if (auto s = backend.RmsNorm(
                k_heads.value(), k_heads.value(), weights_.k_norm, config.rms_norm_eps);
            !s.ok()) {
            return s;
        }
    }

    // 4. RoPE on Q/K
    // Reshape to [1, heads, head_dim]
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
        if (auto s = backend.AppendKV(
                layer_index_, k_reshaped.value(), v_reshaped.value(), position);
            !s.ok()) {
            return s;
        }
        // The query attends to positions [0, position] inclusive.
        if (auto s = backend.AttentionKV(
                attn_ctx_out, q_reshaped.value(), layer_index_, position + 1, attn_cfg);
            !s.ok()) {
            return s;
        }
    } else {
        if (auto s = cache.Append(layer_index_, k_reshaped.value(), v_reshaped.value());
            !s.ok()) {
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

    // 8. Residual add
    if (auto s = backend.AddInPlace(hidden, proj); !s.ok())
        return s;

    // 9. MLP RMSNorm
    auto mlp_norm_out = scratch.AllocateTensor({1, hidden_size}, DType::kF32);
    if (!mlp_norm_out.ok())
        return mlp_norm_out.status();
    auto mlp_out = mlp_norm_out.value();

    if (auto s = backend.RmsNorm(mlp_out, hidden, weights_.mlp_norm, config.rms_norm_eps);
        !s.ok()) {
        return s;
    }

    // 10. Gate / Up projections
    const int32_t inter = config.intermediate_size;
    auto gate_out = scratch.AllocateTensor({1, inter}, DType::kF32);
    if (!gate_out.ok())
        return gate_out.status();
    auto up_out = scratch.AllocateTensor({1, inter}, DType::kF32);
    if (!up_out.ok())
        return up_out.status();
    auto gate = gate_out.value();
    auto up = up_out.value();

    if (auto s = backend.MatMul(gate, mlp_out, weights_.gate_weight_name); !s.ok())
        return s;
    if (auto s = backend.MatMul(up, mlp_out, weights_.up_weight_name); !s.ok())
        return s;

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

} // namespace pl::mllm
