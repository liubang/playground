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
// Attention (host KV path)
// Single-token causal attention with GQA:
//   out[h, d] = sum_j softmax(scale * q[h] . k[j, kv_head]) * v[j, kv_head, d]
// =========================================================================
kernel void mllm_attention(
    device float* out [[buffer(0)]],
    const device float* q     [[buffer(1)]],
    const device float* keys  [[buffer(2)]],
    const device float* values [[buffer(3)]],
    constant uint& num_heads   [[buffer(4)]],
    constant uint& num_kv_heads [[buffer(5)]],
    constant uint& head_dim    [[buffer(6)]],
    constant uint& seq_len     [[buffer(7)]],
    constant float& scale      [[buffer(8)]],
    uint gid [[thread_position_in_grid]])
{
    const uint h = gid / head_dim;
    const uint d = gid % head_dim;
    const uint group = num_heads / num_kv_heads;
    const uint kv_head = h / group;

    const device float* qh = q + (size_t)h * head_dim;
    const device float* kb = keys + (size_t)kv_head * head_dim;
    const device float* vb = values + (size_t)kv_head * head_dim;
    const size_t kv_stride = (size_t)num_kv_heads * head_dim;

    float maxs = -INFINITY;
    for (uint j = 0; j < seq_len; ++j) {
        const device float* kj = kb + (size_t)j * kv_stride;
        float dot = 0.0f;
        for (uint dd = 0; dd < head_dim; ++dd) {
            dot += qh[dd] * kj[dd];
        }
        maxs = max(maxs, dot * scale);
    }

    float sumexp = 0.0f;
    float acc = 0.0f;
    for (uint j = 0; j < seq_len; ++j) {
        const device float* kj = kb + (size_t)j * kv_stride;
        float dot = 0.0f;
        for (uint dd = 0; dd < head_dim; ++dd) {
            dot += qh[dd] * kj[dd];
        }
        const float w = exp(dot * scale - maxs);
        sumexp += w;
        const device float* vj = vb + (size_t)j * kv_stride;
        acc += w * vj[d];
    }
    out[gid] = acc / sumexp;
}

// =========================================================================
// AttentionKV (device KV path)
// Same algorithm as mllm_attention but reads from the device KV buffer
// indexed by layer and capacity.
// KV buffer layout: [num_layers, capacity, num_kv_heads, head_dim]
// =========================================================================
kernel void mllm_attention_kv(
    device float* out [[buffer(0)]],
    const device float* q     [[buffer(1)]],
    const device float* keys  [[buffer(2)]],
    const device float* values [[buffer(3)]],
    constant uint& num_heads   [[buffer(4)]],
    constant uint& num_kv_heads [[buffer(5)]],
    constant uint& head_dim    [[buffer(6)]],
    constant uint& seq_len     [[buffer(7)]],
    constant float& scale      [[buffer(8)]],
    constant uint& capacity    [[buffer(9)]],
    constant uint& layer       [[buffer(10)]],
    uint gid [[thread_position_in_grid]])
{
    const uint h = gid / head_dim;
    const uint d = gid % head_dim;
    const uint group = num_heads / num_kv_heads;
    const uint kv_head = h / group;

    // Layer offset into the KV buffers.
    const size_t layer_offset = (size_t)layer * (size_t)capacity * (size_t)num_kv_heads * head_dim;
    const device float* kb = keys + layer_offset + (size_t)kv_head * head_dim;
    const device float* vb = values + layer_offset + (size_t)kv_head * head_dim;
    const size_t kv_stride = (size_t)num_kv_heads * head_dim;

    const device float* qh = q + (size_t)h * head_dim;

    float maxs = -INFINITY;
    for (uint j = 0; j < seq_len; ++j) {
        const device float* kj = kb + (size_t)j * kv_stride;
        float dot = 0.0f;
        for (uint dd = 0; dd < head_dim; ++dd) {
            dot += qh[dd] * kj[dd];
        }
        maxs = max(maxs, dot * scale);
    }

    float sumexp = 0.0f;
    float acc = 0.0f;
    for (uint j = 0; j < seq_len; ++j) {
        const device float* kj = kb + (size_t)j * kv_stride;
        float dot = 0.0f;
        for (uint dd = 0; dd < head_dim; ++dd) {
            dot += qh[dd] * kj[dd];
        }
        const float w = exp(dot * scale - maxs);
        sumexp += w;
        const device float* vj = vb + (size_t)j * kv_stride;
        acc += w * vj[d];
    }
    out[gid] = acc / sumexp;
}

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
// Q8_0 fused dequant GEMV
// out[o] = sum_i x[i] * dequant(w[o, i])
// w layout (ggml Q8_0): [out_dim][num_blocks] where each block is
//   { fp16 scale (2 bytes), 32 x int8 qs } == 34 bytes.
// One thread per output row; dequant is fused into the MAC loop.
// =========================================================================
kernel void mllm_gemv_q8_0(
    device float* out [[buffer(0)]],
    const device float* x [[buffer(1)]],
    const device uchar* w [[buffer(2)]],
    constant uint& in_dim  [[buffer(3)]],
    constant uint& out_dim [[buffer(4)]],
    uint gid [[thread_position_in_grid]])
{
    if (gid >= out_dim) return;
    const uint num_blocks = in_dim / 32;
    const size_t row_stride = (size_t)num_blocks * 34;
    const device uchar* wrow = w + (size_t)gid * row_stride;
    float acc = 0.0f;
    for (uint blk = 0; blk < num_blocks; ++blk) {
        const device uchar* b = wrow + (size_t)blk * 34;
        const ushort sb = (ushort)b[0] | ((ushort)b[1] << 8);
        const float scale = (float)as_type<half>(sb);
        float dot = 0.0f;
        for (uint j = 0; j < 32; ++j) {
            int q = (int)b[2 + j];
            if (q >= 128) q -= 256;
            dot += x[blk * 32 + j] * (float)q;
        }
        acc += dot * scale;
    }
    out[gid] = acc;
}

// =========================================================================
// f16 GEMV — native half-precision weight reading
// out[o] = sum_i x[i] * w[o, i]  (w is f16)
// Uses simd_half4 for vectorized dot product (4-wide SIMD).
// =========================================================================
kernel void mllm_gemv_f16(
    device float* out [[buffer(0)]],
    const device float* x [[buffer(1)]],
    const device half* w  [[buffer(2)]],
    constant uint& in_dim  [[buffer(3)]],
    constant uint& out_dim [[buffer(4)]],
    uint gid [[thread_position_in_grid]])
{
    if (gid >= out_dim) return;
    const device half* wrow = w + (size_t)gid * in_dim;
    float acc = 0.0f;

    // Vectorized loop: process 4 elements at a time using float4/half4.
    const uint vec_end = in_dim - (in_dim % 4);
    for (uint i = 0; i < vec_end; i += 4) {
        float4 xv = float4(x[i], x[i+1], x[i+2], x[i+3]);
        half4 wv = half4(wrow[i], wrow[i+1], wrow[i+2], wrow[i+3]);
        float4 wvf = float4(wv);
        acc += dot(xv, wvf);
    }
    // Tail
    for (uint i = vec_end; i < in_dim; ++i) {
        acc += x[i] * (float)wrow[i];
    }
    out[gid] = acc;
}

// =========================================================================
// f32 GEMV — plain float weights
// out[o] = sum_i x[i] * w[o, i]  (w is f32)
// =========================================================================
kernel void mllm_gemv_f32(
    device float* out [[buffer(0)]],
    const device float* x [[buffer(1)]],
    const device float* w [[buffer(2)]],
    constant uint& in_dim  [[buffer(3)]],
    constant uint& out_dim [[buffer(4)]],
    uint gid [[thread_position_in_grid]])
{
    if (gid >= out_dim) return;
    const device float* wrow = w + (size_t)gid * in_dim;
    float acc = 0.0f;

    const uint vec_end = in_dim - (in_dim % 4);
    for (uint i = 0; i < vec_end; i += 4) {
        float4 xv = float4(x[i], x[i+1], x[i+2], x[i+3]);
        float4 wv = float4(wrow[i], wrow[i+1], wrow[i+2], wrow[i+3]);
        acc += dot(xv, wv);
    }
    for (uint i = vec_end; i < in_dim; ++i) {
        acc += x[i] * wrow[i];
    }
    out[gid] = acc;
}
)msl";

} // namespace pl::mllm::metal
