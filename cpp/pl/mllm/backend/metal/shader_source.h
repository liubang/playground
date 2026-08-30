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

// Metal Shading Language source for compute kernels. Compiled at runtime via
// MTLCreateSystemDefaultDevice + newLibraryWithSource.
//
// All kernels operate on f32 buffers unless otherwise noted.  The backend
// converts f16/Q8_0 activation inputs to f32 on upload; weight buffers may
// be f16, f32, or Q8_0 (the GEMV kernels handle each natively).

namespace pl::mllm::metal {

inline constexpr const char kShaderSource[] = R"msl(
#include <metal_stdlib>
using namespace metal;

// =========================================================================
// RmsNorm
// out[row, i] = x[row, i] / sqrt(mean(x^2) + eps) * w[i]
// One threadgroup per row; threadgroup reduction for sum of squares.
// =========================================================================
kernel void mllm_rmsnorm(
    device float* out [[buffer(0)]],
    const device float* x [[buffer(1)]],
    const device float* w [[buffer(2)]],
    constant uint& n       [[buffer(3)]],
    constant float& eps    [[buffer(4)]],
    uint tid   [[thread_index_in_threadgroup]],
    uint tgid  [[threadgroup_position_in_grid]],
    uint tsize [[threads_per_threadgroup]],
    threadgroup float* scratch [[threadgroup(0)]])
{
    const uint row = tgid;
    const device float* xrow = x + (size_t)row * n;
    device float* orow = out + (size_t)row * n;

    float sum = 0.0f;
    for (uint i = tid; i < n; i += tsize) {
        const float v = xrow[i];
        sum += v * v;
    }
    scratch[tid] = sum;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint s = tsize / 2; s > 0; s >>= 1) {
        if (tid < s) {
            scratch[tid] += scratch[tid + s];
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    const float denom = rsqrt(scratch[0] / (float)n + eps);
    for (uint i = tid; i < n; i += tsize) {
        orow[i] = xrow[i] * denom * w[i];
    }
}

// =========================================================================
// RoPE — GPT-NeoX style interleaved rotation
//   x'[2i]   = x[2i] * cos(p*theta_i) - x[2i+1] * sin(p*theta_i)
//   x'[2i+1] = x[2i] * sin(p*theta_i) + x[2i+1] * cos(p*theta_i)
//   theta_i  = freq_base^(-2i/head_dim)
// =========================================================================
kernel void mllm_rope(
    device float* q [[buffer(0)]],
    device float* k [[buffer(1)]],
    constant uint& head_dim   [[buffer(2)]],
    constant float& freq_base [[buffer(3)]],
    constant int& position    [[buffer(4)]],
    constant uint& q_elems    [[buffer(5)]],
    constant uint& q_heads    [[buffer(6)]],
    constant uint& kv_heads   [[buffer(7)]],
    uint gid [[thread_position_in_grid]])
{
    const uint q_pairs = q_elems / 2;
    const float p = (float)position;

    device float* ptr;
    uint h, i;
    if (gid < q_pairs) {
        const uint idx = gid * 2;
        const uint rem = idx % (q_heads * head_dim);
        h = rem / head_dim;
        i = (rem % head_dim) / 2;
        ptr = q + idx;
    } else {
        const uint idx = (gid - q_pairs) * 2;
        const uint rem = idx % (kv_heads * head_dim);
        h = rem / head_dim;
        i = (rem % head_dim) / 2;
        ptr = k + idx;
    }

    const float theta = pow(freq_base, (float)(2 * i) / (float)head_dim);
    const float angle = p / theta;
    const float c = cos(angle);
    const float s = sin(angle);
    const float a = ptr[0];
    const float b = ptr[1];
    ptr[0] = a * c - b * s;
    ptr[1] = a * s + b * c;
}

// =========================================================================
// Flash-attention style kernel (single-token decode)
//   One threadgroup per query head; tsize=128 threads cooperate.
//   Processes KV in blocks of BLOCK=128 positions with online softmax:
//     - per block: cooperative score computation, block-max via reduction,
//       block sumexp, then each thread accumulates acc[d_subset] += w*v
//       using threadgroup weights and q-broadcast.
//   Online softmax merges blocks: m_new = max(m_run, m_block);
//     acc *= exp(m_run - m_new); block_partial *= exp(m_block - m_new);
//     acc += block_partial; l likewise.
//   Handles GQA (kv_head = h / group) and arbitrary seq_len with fixed
//   threadgroup memory (~2.5KB for head_dim <= 256).
//
// Layout note: both host and device KV paths share this kernel. The KV base
// offset (in elements) is passed via kv_base; host path passes 0, device path
// passes layer*capacity*num_kv_heads*head_dim.
// =========================================================================
kernel void mllm_attention_flash(
    device float* out            [[buffer(0)]],
    const device float* q        [[buffer(1)]],
    const device float* keys     [[buffer(2)]],
    const device float* values    [[buffer(3)]],
    constant uint& num_heads     [[buffer(4)]],
    constant uint& num_kv_heads  [[buffer(5)]],
    constant uint& head_dim      [[buffer(6)]],
    constant uint& seq_len       [[buffer(7)]],
    constant float& scale        [[buffer(8)]],
    constant ulong& kv_base      [[buffer(9)]],
    uint tid   [[thread_index_in_threadgroup]],
    uint tgid  [[threadgroup_position_in_grid]],
    uint tsize [[threads_per_threadgroup]],
    threadgroup float* tg_q   [[threadgroup(0)]],
    threadgroup float* tg_sc  [[threadgroup(1)]],
    threadgroup float* tg_acc [[threadgroup(2)]],
    threadgroup float* tg_red [[threadgroup(3)]])
{
    constexpr uint BLOCK = 128;
    const uint h = tgid;
    if (h >= num_heads) return;
    const uint group = num_heads / num_kv_heads;
    const uint kv_head = h / group;
    const size_t kv_stride = (size_t)num_kv_heads * head_dim;
    const device float* kb = keys + kv_base + (size_t)kv_head * head_dim;
    const device float* vb = values + kv_base + (size_t)kv_head * head_dim;
    const device float* qh = q + (size_t)h * head_dim;

    // Load q into threadgroup memory for broadcast-friendly access.
    for (uint i = tid; i < head_dim; i += tsize) tg_q[i] = qh[i];
    // Init output accumulator.
    for (uint i = tid; i < head_dim; i += tsize) tg_acc[i] = 0.0f;
    threadgroup_barrier(mem_flags::mem_threadgroup);

    float m_run = -INFINITY;
    float l_run = 0.0f;

    for (uint j0 = 0; j0 < seq_len; j0 += BLOCK) {
        const uint bl = min(BLOCK, seq_len - j0);

        // Stage A: cooperative score computation.
        // Thread tid computes score for position j = j0 + tid (if in range).
        float s = -INFINITY;
        if (tid < bl) {
            const device float* kj = kb + (size_t)(j0 + tid) * kv_stride;
            float dot = 0.0f;
            for (uint dd = 0; dd < head_dim; ++dd) {
                dot += tg_q[dd] * kj[dd];
            }
            s = dot * scale;
        }
        tg_sc[tid] = s;
        threadgroup_barrier(mem_flags::mem_threadgroup);

        // Stage B: block-max via tree reduction.
        tg_red[tid] = s;
        threadgroup_barrier(mem_flags::mem_threadgroup);
        for (uint st = BLOCK / 2; st > 0; st >>= 1) {
            if (tid < st) tg_red[tid] = max(tg_red[tid], tg_red[tid + st]);
            threadgroup_barrier(mem_flags::mem_threadgroup);
        }
        const float m_block = tg_red[0];
        threadgroup_barrier(mem_flags::mem_threadgroup);

        // Stage C: compute weights w = exp(s - m_block); accumulate block sumexp.
        float w = (tid < bl) ? exp(tg_sc[tid] - m_block) : 0.0f;
        tg_sc[tid] = w;
        tg_red[tid] = w;
        threadgroup_barrier(mem_flags::mem_threadgroup);
        for (uint st = BLOCK / 2; st > 0; st >>= 1) {
            if (tid < st) tg_red[tid] += tg_red[tid + st];
            threadgroup_barrier(mem_flags::mem_threadgroup);
        }
        const float sum_block = tg_red[0];
        threadgroup_barrier(mem_flags::mem_threadgroup);

        // Stage D: merge block into running acc using online softmax.
        const float m_new = max(m_run, m_block);
        const float corr_old = exp(m_run - m_new);
        const float corr_new = exp(m_block - m_new);

        // Rescale and add block partial: each thread handles dims
        // d = tid, tid+tsize, tid+2*tsize, ... (coalesced V reads).
        for (uint d = tid; d < head_dim; d += tsize) {
            float p = 0.0f;
            for (uint jj = 0; jj < bl; ++jj) {
                p += tg_sc[jj] * vb[(size_t)(j0 + jj) * kv_stride + d];
            }
            tg_acc[d] = tg_acc[d] * corr_old + p * corr_new;
        }
        l_run = l_run * corr_old + sum_block * corr_new;
        m_run = m_new;
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    // Final write: out[h, d] = acc / l_run.
    for (uint d = tid; d < head_dim; d += tsize) {
        out[(size_t)h * head_dim + d] = tg_acc[d] / l_run;
    }
}

// Legacy mllm_attention_kv is replaced by mllm_attention_flash above.
// Host path and device path both use mllm_attention_flash; the device path
// passes kv_base = layer * capacity * num_kv_heads * head_dim.

// =========================================================================
// SwiGLU
// out[i] = silu(gate[i]) * up[i]
// =========================================================================
kernel void mllm_swiglu(
    device float* out [[buffer(0)]],
    const device float* gate [[buffer(1)]],
    const device float* up   [[buffer(2)]],
    constant uint& n         [[buffer(3)]],
    uint gid [[thread_position_in_grid]])
{
    if (gid >= n) return;
    const float g = gate[gid];
    const float silu = g / (1.0f + exp(-g));
    out[gid] = silu * up[gid];
}

// =========================================================================
// AddInPlace
// x[i] += residual[i]
// =========================================================================
kernel void mllm_add_inplace(
    device float* x [[buffer(0)]],
    const device float* residual [[buffer(1)]],
    constant uint& n             [[buffer(2)]],
    uint gid [[thread_position_in_grid]])
{
    if (gid >= n) return;
    x[gid] += residual[gid];
}

// =========================================================================
// AddBiasInPlace
// x[b, i] += bias[i]
// =========================================================================
kernel void mllm_add_bias(
    device float* x [[buffer(0)]],
    const device float* bias [[buffer(1)]],
    constant uint& n           [[buffer(2)]],
    uint gid [[thread_position_in_grid]])
{
    x[gid] += bias[gid % n];
}

// =========================================================================
// AppendKV — copy one token's K/V into the device KV cache buffer
// KV layout: [num_layers, capacity, num_kv_heads, head_dim]
// src: [num_kv_heads, head_dim] (one token)
// =========================================================================
kernel void mllm_append_kv(
    device float* keys        [[buffer(0)]],
    device float* values      [[buffer(1)]],
    const device float* key   [[buffer(2)]],
    const device float* value [[buffer(3)]],
    constant uint& num_kv_heads [[buffer(4)]],
    constant uint& head_dim    [[buffer(5)]],
    constant uint& capacity    [[buffer(6)]],
    constant uint& layer       [[buffer(7)]],
    constant uint& position    [[buffer(8)]],
    uint gid [[thread_position_in_grid]])
{
    const uint elems = num_kv_heads * head_dim;
    if (gid >= elems) return;

    const size_t layer_offset = (size_t)layer * (size_t)capacity * elems;
    const size_t pos_offset = (size_t)position * elems;
    keys[layer_offset + pos_offset + gid] = key[gid];
    values[layer_offset + pos_offset + gid] = value[gid];
}

// =========================================================================
// Q8_0 fused dequant GEMV (split-K cooperative)
//   out[o] = sum_i x[i] * dequant(w[o, i])
// w layout (ggml Q8_0): [out_dim][num_blocks], block = {fp16 scale, 32 int8}
//   == 34 bytes.
// NT threads cooperate per output row (split-K); threadgroup reduction sums
// the NT partials. With NT=4 and tsize=256, each group covers 64 output rows,
// giving out_dim*4 threads → ~16x more threadgroups than the per-row kernel
// at out_dim=4096, dramatically improving GPU occupancy on Apple GPUs.
// =========================================================================
kernel void mllm_gemv_q8_0(
    device float* out        [[buffer(0)]],
    const device float* x    [[buffer(1)]],
    const device uchar* w    [[buffer(2)]],
    constant uint& in_dim    [[buffer(3)]],
    constant uint& out_dim   [[buffer(4)]],
    uint tid   [[thread_index_in_threadgroup]],
    uint tgid  [[threadgroup_position_in_grid]],
    uint tsize [[threads_per_threadgroup]],
    threadgroup float* partial [[threadgroup(0)]])
{
    constexpr uint NT = 4;
    const uint rows_per_group = tsize / NT;
    const uint lane = tid % NT;
    const uint row = tgid * rows_per_group + (tid / NT);

    float acc = 0.0f;
    if (row < out_dim) {
        const uint num_blocks = in_dim / 32;
        const size_t row_stride = (size_t)num_blocks * 34;
        const device uchar* wrow = w + (size_t)row * row_stride;
        for (uint blk = lane; blk < num_blocks; blk += NT) {
            const device uchar* b = wrow + (size_t)blk * 34;
            const ushort sb = (ushort)b[0] | ((ushort)b[1] << 8);
            const float scale = (float)as_type<half>(sb);
            float dot = 0.0f;
            #pragma unroll
            for (uint j = 0; j < 32; ++j) {
                int q = (int)b[2 + j];
                if (q >= 128) q -= 256;
                dot += x[blk * 32 + j] * (float)q;
            }
            acc += dot * scale;
        }
    }
    partial[tid] = acc;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lane == 0 && row < out_dim) {
        const uint base = (tid / NT) * NT;
        out[row] = partial[base] + partial[base + 1]
                 + partial[base + 2] + partial[base + 3];
    }
}

// =========================================================================
// f16 GEMV (split-K cooperative)
//   out[o] = sum_i x[i] * w[o, i]  (w is f16)
// NT threads cooperate per output row using half4/float4 vectorized loads;
// threadgroup reduction sums partials. NT=4, tsize=256 → 64 rows/group.
// =========================================================================
kernel void mllm_gemv_f16(
    device float* out        [[buffer(0)]],
    const device float* x    [[buffer(1)]],
    const device half* w     [[buffer(2)]],
    constant uint& in_dim    [[buffer(3)]],
    constant uint& out_dim   [[buffer(4)]],
    uint tid   [[thread_index_in_threadgroup]],
    uint tgid  [[threadgroup_position_in_grid]],
    uint tsize [[threads_per_threadgroup]],
    threadgroup float* partial [[threadgroup(0)]])
{
    constexpr uint NT = 4;
    const uint rows_per_group = tsize / NT;
    const uint lane = tid % NT;
    const uint row = tgid * rows_per_group + (tid / NT);

    float acc = 0.0f;
    if (row < out_dim) {
        const device half* wrow = w + (size_t)row * in_dim;
        // Each lane processes chunks of 4 elements, strided by NT*4.
        const uint vec_end = in_dim - (in_dim % 4);
        for (uint i = lane * 4; i < vec_end; i += NT * 4) {
            float4 xv = float4(x[i], x[i + 1], x[i + 2], x[i + 3]);
            half4 wv = half4(wrow[i], wrow[i + 1], wrow[i + 2], wrow[i + 3]);
            acc += dot(xv, float4(wv));
        }
        // Tail (assign to lane 0 only).
        if (lane == 0) {
            for (uint i = vec_end; i < in_dim; ++i) {
                acc += x[i] * (float)wrow[i];
            }
        }
    }
    partial[tid] = acc;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lane == 0 && row < out_dim) {
        const uint base = (tid / NT) * NT;
        out[row] = partial[base] + partial[base + 1]
                 + partial[base + 2] + partial[base + 3];
    }
}

// =========================================================================
// f32 GEMV (split-K cooperative)
//   out[o] = sum_i x[i] * w[o, i]  (w is f32)
// NT threads cooperate per output row using float4 vectorized loads;
// threadgroup reduction sums partials. NT=4, tsize=256 → 64 rows/group.
// =========================================================================
kernel void mllm_gemv_f32(
    device float* out        [[buffer(0)]],
    const device float* x    [[buffer(1)]],
    const device float* w    [[buffer(2)]],
    constant uint& in_dim    [[buffer(3)]],
    constant uint& out_dim   [[buffer(4)]],
    uint tid   [[thread_index_in_threadgroup]],
    uint tgid  [[threadgroup_position_in_grid]],
    uint tsize [[threads_per_threadgroup]],
    threadgroup float* partial [[threadgroup(0)]])
{
    constexpr uint NT = 4;
    const uint rows_per_group = tsize / NT;
    const uint lane = tid % NT;
    const uint row = tgid * rows_per_group + (tid / NT);

    float acc = 0.0f;
    if (row < out_dim) {
        const device float* wrow = w + (size_t)row * in_dim;
        const uint vec_end = in_dim - (in_dim % 4);
        for (uint i = lane * 4; i < vec_end; i += NT * 4) {
            float4 xv = float4(x[i], x[i + 1], x[i + 2], x[i + 3]);
            float4 wv = float4(wrow[i], wrow[i + 1], wrow[i + 2], wrow[i + 3]);
            acc += dot(xv, wv);
        }
        if (lane == 0) {
            for (uint i = vec_end; i < in_dim; ++i) {
                acc += x[i] * wrow[i];
            }
        }
    }
    partial[tid] = acc;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lane == 0 && row < out_dim) {
        const uint base = (tid / NT) * NT;
        out[row] = partial[base] + partial[base + 1]
                 + partial[base + 2] + partial[base + 3];
    }
}
)msl";

} // namespace pl::mllm::metal
