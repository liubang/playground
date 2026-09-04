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

#include <cmath>
#include <cstring>
#include <gtest/gtest.h>
#include <random>
#include <vector>

#include "cpp/pl/mllm/backend/cpu/cpu_backend.h"
#include "cpp/pl/mllm/core/buffer.h"
#include "cpp/pl/mllm/core/dtype.h"
#include "cpp/pl/mllm/core/tensor.h"

namespace pl::mllm {
namespace {

// Test helpers

struct HostTensor {
    OwnedBuffer buf;
    TensorView view;

    // HostTensor for plain (non-quantized) dtypes.
    static HostTensor alloc(std::initializer_list<int64_t> dims, DType dtype) {
        Shape shape(dims);
        int64_t numel = shape.numel();
        size_t bytes = dtype_nbytes(dtype, numel);
        EXPECT_GT(bytes, 0u);
        auto buf = OwnedBuffer::AllocateCpu(bytes, 64).value();
        std::memset(buf.data(), 0, bytes);
        TensorView view(buf.data(), dtype, shape);
        return {std::move(buf), view};
    }

    // HostTensor for Q8_0: numel must be block-aligned.
    static HostTensor alloc_q8_0(std::initializer_list<int64_t> dims) {
        Shape shape(dims);
        int64_t numel = shape.numel();
        EXPECT_EQ(numel % kQ8_0BlockSize, 0);
        size_t bytes = dtype_nbytes(DType::kQ8_0, numel);
        auto buf = OwnedBuffer::AllocateCpu(bytes, 64).value();
        std::memset(buf.data(), 0, bytes);
        TensorView view(buf.data(), DType::kQ8_0, shape);
        return {std::move(buf), view};
    }

    // HostTensor for Q4_0: numel must be block-aligned.
    static HostTensor alloc_q4_0(std::initializer_list<int64_t> dims) {
        Shape shape(dims);
        int64_t numel = shape.numel();
        EXPECT_EQ(numel % kQ4_0BlockSize, 0);
        size_t bytes = dtype_nbytes(DType::kQ4_0, numel);
        auto buf = OwnedBuffer::AllocateCpu(bytes, 64).value();
        std::memset(buf.data(), 0, bytes);
        TensorView view(buf.data(), DType::kQ4_0, shape);
        return {std::move(buf), view};
    }
};

// Q4_0 block (ggml layout): fp16 scale + 16 packed nibbles.
// Low nibbles -> elements [0,16), high nibbles -> [16,32).
struct Q4Block {
    uint16_t scale;
    uint8_t qs[16];
};
static_assert(sizeof(Q4Block) == kQ4_0TypeSize);

// Q8_0 block (ggml layout).
struct Q8Block {
    uint16_t scale;
    int8_t qs[32];
};
static_assert(sizeof(Q8Block) == kQ8_0TypeSize);

// Quantize one 32-element block (ggml quantize_row_q4_0_ref semantics):
// scale = max_abs / -8 so the symmetric range [-8, 7] covers the data.
void quantize_q4_0_block(const float* src, Q4Block* dst) {
    float max_abs = 0.0f;
    for (int i = 0; i < 32; ++i) {
        max_abs = std::max(max_abs, std::abs(src[i]));
    }
    const float scale = max_abs >= 1e-8f ? max_abs / -8.0f : 1.0f;
    dst->scale = fp32_to_fp16(scale);
    for (int j = 0; j < 16; ++j) {
        const float v0 = src[j] / scale;
        const float v1 = src[j + 16] / scale;
        const int q0 = std::max(-8, std::min(7, static_cast<int>(std::round(v0))));
        const int q1 = std::max(-8, std::min(7, static_cast<int>(std::round(v1))));
        dst->qs[j] = static_cast<uint8_t>((q0 + 8) | ((q1 + 8) << 4));
    }
}

// Dequantize a Q4_0 block back to f32.
void dequantize_q4_0_block(const Q4Block* src, float* dst) {
    const float scale = fp16_to_fp32(src->scale);
    for (int j = 0; j < 16; ++j) {
        dst[j] = scale * static_cast<float>(static_cast<int>(src->qs[j] & 0x0F) - 8);
        dst[j + 16] = scale * static_cast<float>(static_cast<int>(src->qs[j] >> 4) - 8);
    }
}

// Naive matmul reference (all f32).
void ref_matmul(float* out, const float* x, const float* w, int batch, int out_dim, int in_dim) {
    for (int b = 0; b < batch; ++b) {
        for (int o = 0; o < out_dim; ++o) {
            float acc = 0.0f;
            for (int i = 0; i < in_dim; ++i) {
                acc += x[b * in_dim + i] * w[o * in_dim + i];
            }
            out[b * out_dim + o] = acc;
        }
    }
}

// Naive RMSNorm reference.
void ref_rmsnorm(float* out, const float* x, const float* w, int batch, int hidden, float eps) {
    const float inv = 1.0f / static_cast<float>(hidden);
    for (int b = 0; b < batch; ++b) {
        float ms = 0.0f;
        for (int i = 0; i < hidden; ++i) {
            ms += x[b * hidden + i] * x[b * hidden + i];
        }
        ms *= inv;
        const float denom = 1.0f / std::sqrt(ms + eps);
        for (int i = 0; i < hidden; ++i) {
            out[b * hidden + i] = x[b * hidden + i] * denom * w[i];
        }
    }
}

// Naive RoPE reference (GPT-NeoX half-split style, as used by LLaMA / Qwen).
void ref_rope(float* q,
              float* k,
              int batch,
              int num_heads,
              int num_kv_heads,
              int head_dim,
              int64_t position,
              float freq_base) {
    auto apply = [&](float* ptr) {
        const float p = static_cast<float>(position);
        const int half = head_dim / 2;
        for (int i = 0; i < half; ++i) {
            const float theta =
                1.0f /
                std::pow(freq_base, static_cast<float>(2 * i) / static_cast<float>(head_dim));
            const float angle = p * theta;
            const float c = std::cos(angle);
            const float s = std::sin(angle);
            const float a = ptr[i];
            const float b = ptr[i + half];
            ptr[i] = a * c - b * s;
            ptr[i + half] = a * s + b * c;
        }
    };
    for (int b = 0; b < batch; ++b) {
        for (int h = 0; h < num_heads; ++h) {
            apply(q + b * num_heads * head_dim + h * head_dim);
        }
        for (int h = 0; h < num_kv_heads; ++h) {
            apply(k + b * num_kv_heads * head_dim + h * head_dim);
        }
    }
}

// Naive attention reference.
void ref_attention(float* out,
                   const float* q,
                   const float* k,
                   const float* v,
                   int num_heads,
                   int num_kv_heads,
                   int head_dim,
                   int seq_len) {
    const int group = num_heads / num_kv_heads;
    const float scale = 1.0f / std::sqrt(static_cast<float>(head_dim));

    for (int h = 0; h < num_heads; ++h) {
        const int kv_head = h / group;
        std::vector<float> scores(seq_len);
        float max_score = -1e30f;
        for (int j = 0; j < seq_len; ++j) {
            float dot = 0.0f;
            for (int d = 0; d < head_dim; ++d) {
                dot +=
                    q[h * head_dim + d] * k[j * num_kv_heads * head_dim + kv_head * head_dim + d];
            }
            scores[j] = dot * scale;
            if (scores[j] > max_score)
                max_score = scores[j];
        }
        float sum = 0.0f;
        for (int j = 0; j < seq_len; ++j) {
            scores[j] = std::exp(scores[j] - max_score);
            sum += scores[j];
        }
        for (int d = 0; d < head_dim; ++d) {
            float acc = 0.0f;
            for (int j = 0; j < seq_len; ++j) {
                acc += scores[j] * v[j * num_kv_heads * head_dim + kv_head * head_dim + d];
            }
            out[h * head_dim + d] = acc / sum;
        }
    }
}

void ref_swiglu(float* out, const float* gate, const float* up, int n) {
    for (int i = 0; i < n; ++i) {
        const float g = gate[i];
        const float silu = g / (1.0f + std::exp(-g));
        out[i] = silu * up[i];
    }
}

// MatMul tests

TEST(CpuBackendTest, MatMulF32) {
    const int batch = 1, in_dim = 8, out_dim = 4;
    auto x = HostTensor::alloc({batch, in_dim}, DType::kF32);
    auto w = HostTensor::alloc({out_dim, in_dim}, DType::kF32);
    auto out = HostTensor::alloc({batch, out_dim}, DType::kF32);

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (int i = 0; i < batch * in_dim; ++i)
        x.view.data_as<float>()[i] = dist(rng);
    for (int i = 0; i < out_dim * in_dim; ++i)
        w.view.data_as<float>()[i] = dist(rng);

    CpuBackend backend;
    std::array names = {std::string_view{"w0"}};
    std::array<TensorView, 1> views = {w.view};
    ASSERT_TRUE(backend.ImportWeights(views, names).ok());

    ASSERT_TRUE(backend.MatMul(out.view, x.view, "w0").ok());

    std::vector<float> expected(batch * out_dim);
    ref_matmul(
        expected.data(), x.view.data_as<float>(), w.view.data_as<float>(), batch, out_dim, in_dim);

    for (int i = 0; i < batch * out_dim; ++i) {
        EXPECT_NEAR(out.view.data_as<float>()[i], expected[i], 1e-5f);
    }
}

TEST(CpuBackendTest, MatMulF16Weight) {
    const int batch = 1, in_dim = 8, out_dim = 4;
    auto x = HostTensor::alloc({batch, in_dim}, DType::kF32);
    auto w = HostTensor::alloc({out_dim, in_dim}, DType::kF16);
    auto out = HostTensor::alloc({batch, out_dim}, DType::kF32);

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (int i = 0; i < batch * in_dim; ++i)
        x.view.data_as<float>()[i] = dist(rng);

    // Fill f16 weight with known values
    auto* wh = w.view.data_as<uint16_t>();
    std::vector<float> w_f32(out_dim * in_dim);
    for (int i = 0; i < out_dim * in_dim; ++i) {
        w_f32[i] = dist(rng);
        wh[i] = fp32_to_fp16(w_f32[i]);
    }

    CpuBackend backend;
    std::array names = {std::string_view{"w0"}};
    std::array<TensorView, 1> views = {w.view};
    ASSERT_TRUE(backend.ImportWeights(views, names).ok());

    ASSERT_TRUE(backend.MatMul(out.view, x.view, "w0").ok());

    std::vector<float> expected(batch * out_dim);
    ref_matmul(expected.data(), x.view.data_as<float>(), w_f32.data(), batch, out_dim, in_dim);

    for (int i = 0; i < batch * out_dim; ++i) {
        EXPECT_NEAR(out.view.data_as<float>()[i], expected[i], 2e-2f);
    }
}

TEST(CpuBackendTest, MatMulQ8_0) {
    const int batch = 1, in_dim = 32, out_dim = 4;
    auto x = HostTensor::alloc({batch, in_dim}, DType::kF32);
    auto w = HostTensor::alloc_q8_0({out_dim, in_dim});
    auto out = HostTensor::alloc({batch, out_dim}, DType::kF32);

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    // Fill input with random values
    for (int i = 0; i < batch * in_dim; ++i) {
        x.view.data_as<float>()[i] = dist(rng);
    }

    // Create a Q8_0 weight block from random f32 data (see Q8Block helper)
    auto* blocks = static_cast<Q8Block*>(w.buf.data());
    std::vector<float> w_f32(out_dim * in_dim);
    const int num_blocks = in_dim / 32;

    for (int o = 0; o < out_dim; ++o) {
        for (int blk = 0; blk < num_blocks; ++blk) {
            // Generate random values for this block
            float max_abs = 0.0f;
            for (int j = 0; j < 32; ++j) {
                const float v = dist(rng);
                w_f32[o * in_dim + blk * 32 + j] = v;
                max_abs = std::max(max_abs, std::abs(v));
            }
            // Quantize: scale = max_abs / 127
            const float scale = max_abs / 127.0f;
            blocks[o * num_blocks + blk].scale = fp32_to_fp16(scale);
            for (int j = 0; j < 32; ++j) {
                const int q =
                    static_cast<int>(std::round(w_f32[o * in_dim + blk * 32 + j] / scale));
                blocks[o * num_blocks + blk].qs[j] =
                    static_cast<int8_t>(std::max(-128, std::min(127, q)));
            }
        }
    }

    CpuBackend backend;
    std::array names = {std::string_view{"w0"}};
    std::array<TensorView, 1> views = {w.view};
    ASSERT_TRUE(backend.ImportWeights(views, names).ok());

    ASSERT_TRUE(backend.MatMul(out.view, x.view, "w0").ok());

    // Reference: dequantize Q8_0 and compute naive matmul
    std::vector<float> w_dequant(out_dim * in_dim);
    for (int o = 0; o < out_dim; ++o) {
        for (int blk = 0; blk < num_blocks; ++blk) {
            const float scale = fp16_to_fp32(blocks[o * num_blocks + blk].scale);
            for (int j = 0; j < 32; ++j) {
                w_dequant[o * in_dim + blk * 32 + j] =
                    scale * static_cast<float>(blocks[o * num_blocks + blk].qs[j]);
            }
        }
    }

    std::vector<float> expected(batch * out_dim);
    ref_matmul(expected.data(), x.view.data_as<float>(), w_dequant.data(), batch, out_dim, in_dim);

    // Q8_0 quantization introduces ~1% error
    for (int i = 0; i < batch * out_dim; ++i) {
        EXPECT_NEAR(
            out.view.data_as<float>()[i], expected[i], std::abs(expected[i]) * 0.02f + 1e-3f);
    }
}

TEST(CpuBackendTest, MatMulQ4_0) {
    const int batch = 1, in_dim = 64, out_dim = 4;
    auto x = HostTensor::alloc({batch, in_dim}, DType::kF32);
    auto w = HostTensor::alloc_q4_0({out_dim, in_dim});
    auto out = HostTensor::alloc({batch, out_dim}, DType::kF32);

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (int i = 0; i < batch * in_dim; ++i) {
        x.view.data_as<float>()[i] = dist(rng);
    }

    auto* blocks = static_cast<Q4Block*>(w.buf.data());
    const int num_blocks = in_dim / static_cast<int>(kQ4_0BlockSize);
    std::vector<float> w_src(out_dim * in_dim);
    for (int o = 0; o < out_dim; ++o) {
        for (int blk = 0; blk < num_blocks; ++blk) {
            for (int j = 0; j < 32; ++j) {
                w_src[o * in_dim + blk * 32 + j] = dist(rng);
            }
            quantize_q4_0_block(w_src.data() + o * in_dim + blk * 32,
                                &blocks[o * num_blocks + blk]);
        }
    }

    CpuBackend backend;
    std::array names = {std::string_view{"w0"}};
    std::array<TensorView, 1> views = {w.view};
    ASSERT_TRUE(backend.ImportWeights(views, names).ok());

    ASSERT_TRUE(backend.MatMul(out.view, x.view, "w0").ok());

    // Reference: dequantize Q4_0 and compute naive matmul.
    std::vector<float> w_dequant(out_dim * in_dim);
    for (int o = 0; o < out_dim; ++o) {
        for (int blk = 0; blk < num_blocks; ++blk) {
            dequantize_q4_0_block(&blocks[o * num_blocks + blk],
                                  w_dequant.data() + o * in_dim + blk * 32);
        }
    }

    std::vector<float> expected(batch * out_dim);
    ref_matmul(expected.data(), x.view.data_as<float>(), w_dequant.data(), batch, out_dim, in_dim);

    // Exact block decomposition: both do sum over (dot * scale) per block.
    for (int i = 0; i < batch * out_dim; ++i) {
        EXPECT_NEAR(
            out.view.data_as<float>()[i], expected[i], std::abs(expected[i]) * 1e-5f + 1e-5f);
    }

    // Quantized result must also track the original f32 weight. Q4_0 uses a
    // 4-bit grid (step = max_abs/8), so allow the looser ~15% relative that
    // per-element rounding can produce after summation.
    std::vector<float> ideal(batch * out_dim);
    ref_matmul(ideal.data(), x.view.data_as<float>(), w_src.data(), batch, out_dim, in_dim);
    for (int i = 0; i < batch * out_dim; ++i) {
        EXPECT_NEAR(out.view.data_as<float>()[i], ideal[i], std::abs(ideal[i]) * 0.15f + 1e-3f);
    }
}

TEST(CpuBackendTest, MatMulBatch) {
    const int batch = 3, in_dim = 4, out_dim = 2;
    auto x = HostTensor::alloc({batch, in_dim}, DType::kF32);
    auto w = HostTensor::alloc({out_dim, in_dim}, DType::kF32);
    auto out = HostTensor::alloc({batch, out_dim}, DType::kF32);

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (int i = 0; i < batch * in_dim; ++i)
        x.view.data_as<float>()[i] = dist(rng);
    for (int i = 0; i < out_dim * in_dim; ++i)
        w.view.data_as<float>()[i] = dist(rng);

    CpuBackend backend;
    std::array names = {std::string_view{"w0"}};
    std::array<TensorView, 1> views = {w.view};
    ASSERT_TRUE(backend.ImportWeights(views, names).ok());

    ASSERT_TRUE(backend.MatMul(out.view, x.view, "w0").ok());

    std::vector<float> expected(batch * out_dim);
    ref_matmul(
        expected.data(), x.view.data_as<float>(), w.view.data_as<float>(), batch, out_dim, in_dim);

    for (int i = 0; i < batch * out_dim; ++i) {
        EXPECT_NEAR(out.view.data_as<float>()[i], expected[i], 1e-5f);
    }
}

// RMSNorm tests

TEST(CpuBackendTest, RmsNorm) {
    const int batch = 1, hidden = 8;
    auto x = HostTensor::alloc({batch, hidden}, DType::kF32);
    auto w = HostTensor::alloc({hidden}, DType::kF32);
    auto out = HostTensor::alloc({batch, hidden}, DType::kF32);

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(0.1f, 1.0f);
    for (int i = 0; i < hidden; ++i) {
        x.view.data_as<float>()[i] = dist(rng);
        w.view.data_as<float>()[i] = dist(rng);
    }

    CpuBackend backend;
    const float eps = 1e-5f;
    ASSERT_TRUE(backend.RmsNorm(out.view, x.view, w.view, eps).ok());

    std::vector<float> expected(hidden);
    ref_rmsnorm(
        expected.data(), x.view.data_as<float>(), w.view.data_as<float>(), batch, hidden, eps);

    for (int i = 0; i < hidden; ++i) {
        EXPECT_NEAR(out.view.data_as<float>()[i], expected[i], 1e-5f);
    }
}

TEST(CpuBackendTest, RmsNormF16Input) {
    const int batch = 1, hidden = 8;
    auto x = HostTensor::alloc({batch, hidden}, DType::kF16);
    auto w = HostTensor::alloc({hidden}, DType::kF16);
    auto out = HostTensor::alloc({batch, hidden}, DType::kF32);

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(0.1f, 1.0f);

    std::vector<float> x_f32(hidden), w_f32(hidden);
    for (int i = 0; i < hidden; ++i) {
        x_f32[i] = dist(rng);
        w_f32[i] = dist(rng);
        x.view.data_as<uint16_t>()[i] = fp32_to_fp16(x_f32[i]);
        w.view.data_as<uint16_t>()[i] = fp32_to_fp16(w_f32[i]);
    }

    CpuBackend backend;
    const float eps = 1e-5f;
    ASSERT_TRUE(backend.RmsNorm(out.view, x.view, w.view, eps).ok());

    std::vector<float> expected(hidden);
    ref_rmsnorm(expected.data(), x_f32.data(), w_f32.data(), batch, hidden, eps);

    for (int i = 0; i < hidden; ++i) {
        EXPECT_NEAR(out.view.data_as<float>()[i], expected[i], 2e-2f);
    }
}

// RoPE tests

TEST(CpuBackendTest, RoPEF32) {
    const int batch = 1, num_heads = 2, num_kv_heads = 1, head_dim = 8;
    auto q = HostTensor::alloc({batch, num_heads, head_dim}, DType::kF32);
    auto k = HostTensor::alloc({batch, num_kv_heads, head_dim}, DType::kF32);

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    std::vector<float> q_f32(batch * num_heads * head_dim);
    std::vector<float> k_f32(batch * num_kv_heads * head_dim);
    for (int i = 0; i < batch * num_heads * head_dim; ++i) {
        q_f32[i] = dist(rng);
        q.view.data_as<float>()[i] = q_f32[i];
    }
    for (int i = 0; i < batch * num_kv_heads * head_dim; ++i) {
        k_f32[i] = dist(rng);
        k.view.data_as<float>()[i] = k_f32[i];
    }

    CpuBackend backend;
    RopeConfig config{.head_dim = head_dim, .freq_base = 10000.0f};
    const int64_t position = 5;
    ASSERT_TRUE(backend.RoPE(q.view, k.view, position, config).ok());

    // Apply reference
    ref_rope(
        q_f32.data(), k_f32.data(), batch, num_heads, num_kv_heads, head_dim, position, 10000.0f);

    for (int i = 0; i < batch * num_heads * head_dim; ++i) {
        EXPECT_NEAR(q.view.data_as<float>()[i], q_f32[i], 1e-5f);
    }
    for (int i = 0; i < batch * num_kv_heads * head_dim; ++i) {
        EXPECT_NEAR(k.view.data_as<float>()[i], k_f32[i], 1e-5f);
    }
}

TEST(CpuBackendTest, RoPEPositionZero) {
    const int batch = 1, num_heads = 1, num_kv_heads = 1, head_dim = 4;
    auto q = HostTensor::alloc({batch, num_heads, head_dim}, DType::kF32);
    auto k = HostTensor::alloc({batch, num_kv_heads, head_dim}, DType::kF32);

    std::vector<float> q_orig = {1.0f, 2.0f, 3.0f, 4.0f};
    std::vector<float> k_orig = {5.0f, 6.0f, 7.0f, 8.0f};
    std::memcpy(q.view.data(), q_orig.data(), sizeof(float) * 4);
    std::memcpy(k.view.data(), k_orig.data(), sizeof(float) * 4);

    CpuBackend backend;
    RopeConfig config{.head_dim = head_dim, .freq_base = 10000.0f};
    ASSERT_TRUE(backend.RoPE(q.view, k.view, 0, config).ok());

    // Position 0: cos(0) = 1, sin(0) = 0, so tensors should be unchanged
    for (int i = 0; i < 4; ++i) {
        EXPECT_NEAR(q.view.data_as<float>()[i], q_orig[i], 1e-6f);
        EXPECT_NEAR(k.view.data_as<float>()[i], k_orig[i], 1e-6f);
    }
}

// Attention tests

TEST(CpuBackendTest, AttentionF32) {
    const int num_heads = 2, num_kv_heads = 1, head_dim = 4, seq_len = 3;
    auto q = HostTensor::alloc({1, num_heads, head_dim}, DType::kF32);
    auto out = HostTensor::alloc({1, num_heads * head_dim}, DType::kF32);

    // KV cache in f32
    std::vector<float> k_data(seq_len * num_kv_heads * head_dim);
    std::vector<float> v_data(seq_len * num_kv_heads * head_dim);

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (int i = 0; i < num_heads * head_dim; ++i) {
        q.view.data_as<float>()[i] = dist(rng);
    }
    for (int i = 0; i < seq_len * num_kv_heads * head_dim; ++i) {
        k_data[i] = dist(rng);
        v_data[i] = dist(rng);
    }

    KVCacheView kv{
        .keys = k_data.data(),
        .values = v_data.data(),
        .seq_len = seq_len,
        .num_kv_heads = num_kv_heads,
        .head_dim = head_dim,
        .dtype = DType::kF32,
    };

    AttentionConfig config{
        .num_heads = num_heads,
        .num_kv_heads = num_kv_heads,
        .head_dim = head_dim,
        .scale = 0.0f, // auto
    };

    CpuBackend backend;
    ASSERT_TRUE(backend.Attention(out.view, q.view, kv, config).ok());

    std::vector<float> expected(num_heads * head_dim);
    ref_attention(expected.data(),
                  q.view.data_as<float>(),
                  k_data.data(),
                  v_data.data(),
                  num_heads,
                  num_kv_heads,
                  head_dim,
                  seq_len);

    for (int i = 0; i < num_heads * head_dim; ++i) {
        EXPECT_NEAR(out.view.data_as<float>()[i], expected[i], 1e-5f);
    }
}

TEST(CpuBackendTest, AttentionSeqLenOne) {
    const int num_heads = 1, num_kv_heads = 1, head_dim = 4, seq_len = 1;
    auto q = HostTensor::alloc({1, num_heads, head_dim}, DType::kF32);
    auto out = HostTensor::alloc({1, num_heads * head_dim}, DType::kF32);

    std::vector<float> k_data = {1.0f, 2.0f, 3.0f, 4.0f};
    std::vector<float> v_data = {5.0f, 6.0f, 7.0f, 8.0f};
    std::vector<float> q_data = {0.5f, 0.5f, 0.5f, 0.5f};
    std::memcpy(q.view.data(), q_data.data(), sizeof(float) * 4);

    KVCacheView kv{
        .keys = k_data.data(),
        .values = v_data.data(),
        .seq_len = seq_len,
        .num_kv_heads = num_kv_heads,
        .head_dim = head_dim,
        .dtype = DType::kF32,
    };

    AttentionConfig config{
        .num_heads = num_heads,
        .num_kv_heads = num_kv_heads,
        .head_dim = head_dim,
        .scale = 0.0f,
    };

    CpuBackend backend;
    ASSERT_TRUE(backend.Attention(out.view, q.view, kv, config).ok());

    // With seq_len=1, softmax weight = 1.0, so out = v
    for (int i = 0; i < head_dim; ++i) {
        EXPECT_NEAR(out.view.data_as<float>()[i], v_data[i], 1e-6f);
    }
}

// SwiGLU tests

TEST(CpuBackendTest, SwiGLU) {
    const int n = 8;
    auto gate = HostTensor::alloc({1, n}, DType::kF32);
    auto up = HostTensor::alloc({1, n}, DType::kF32);
    auto out = HostTensor::alloc({1, n}, DType::kF32);

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-2.0f, 2.0f);
    for (int i = 0; i < n; ++i) {
        gate.view.data_as<float>()[i] = dist(rng);
        up.view.data_as<float>()[i] = dist(rng);
    }

    CpuBackend backend;
    ASSERT_TRUE(backend.SwiGLU(out.view, gate.view, up.view).ok());

    std::vector<float> expected(n);
    ref_swiglu(expected.data(), gate.view.data_as<float>(), up.view.data_as<float>(), n);

    for (int i = 0; i < n; ++i) {
        EXPECT_NEAR(out.view.data_as<float>()[i], expected[i], 1e-5f);
    }
}

// AddInPlace tests

TEST(CpuBackendTest, AddInPlaceF32) {
    const int n = 8;
    auto x = HostTensor::alloc({1, n}, DType::kF32);
    auto residual = HostTensor::alloc({1, n}, DType::kF32);

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> x_orig(n), r_orig(n);
    for (int i = 0; i < n; ++i) {
        x_orig[i] = dist(rng);
        r_orig[i] = dist(rng);
        x.view.data_as<float>()[i] = x_orig[i];
        residual.view.data_as<float>()[i] = r_orig[i];
    }

    CpuBackend backend;
    ASSERT_TRUE(backend.AddInPlace(x.view, residual.view).ok());

    for (int i = 0; i < n; ++i) {
        EXPECT_NEAR(x.view.data_as<float>()[i], x_orig[i] + r_orig[i], 1e-6f);
    }
}

TEST(CpuBackendTest, AddInPlaceF16) {
    const int n = 8;
    auto x = HostTensor::alloc({1, n}, DType::kF16);
    auto residual = HostTensor::alloc({1, n}, DType::kF32);

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> x_orig(n), r_orig(n);
    for (int i = 0; i < n; ++i) {
        x_orig[i] = dist(rng);
        r_orig[i] = dist(rng);
        x.view.data_as<uint16_t>()[i] = fp32_to_fp16(x_orig[i]);
        residual.view.data_as<float>()[i] = r_orig[i];
    }

    CpuBackend backend;
    ASSERT_TRUE(backend.AddInPlace(x.view, residual.view).ok());

    for (int i = 0; i < n; ++i) {
        const float expected = fp16_to_fp32(fp32_to_fp16(x_orig[i] + r_orig[i]));
        EXPECT_NEAR(fp16_to_fp32(x.view.data_as<uint16_t>()[i]), expected, 1e-2f);
    }
}

// Error handling tests

TEST(CpuBackendTest, MatMulWeightNotFound) {
    auto x = HostTensor::alloc({1, 4}, DType::kF32);
    auto out = HostTensor::alloc({1, 4}, DType::kF32);
    CpuBackend backend;
    auto s = backend.MatMul(out.view, x.view, "nonexistent");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code, ErrorCode::kNotFound);
}

TEST(CpuBackendTest, MatMulShapeMismatch) {
    auto x = HostTensor::alloc({1, 4}, DType::kF32);
    auto w = HostTensor::alloc({8, 4}, DType::kF32);
    auto out = HostTensor::alloc({1, 2}, DType::kF32); // wrong out_dim

    CpuBackend backend;
    std::array names = {std::string_view{"w0"}};
    std::array<TensorView, 1> views = {w.view};
    ASSERT_TRUE(backend.ImportWeights(views, names).ok());

    auto s = backend.MatMul(out.view, x.view, "w0");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code, ErrorCode::kInvalidArgument);
}

TEST(CpuBackendTest, RmsNormBadEps) {
    auto x = HostTensor::alloc({1, 4}, DType::kF32);
    auto w = HostTensor::alloc({4}, DType::kF32);
    auto out = HostTensor::alloc({1, 4}, DType::kF32);
    CpuBackend backend;
    auto s = backend.RmsNorm(out.view, x.view, w.view, 0.0f);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code, ErrorCode::kInvalidArgument);
}

TEST(CpuBackendTest, RoPEOddHeadDim) {
    auto q = HostTensor::alloc({1, 1, 3}, DType::kF32);
    auto k = HostTensor::alloc({1, 1, 3}, DType::kF32);
    CpuBackend backend;
    RopeConfig config{.head_dim = 3, .freq_base = 10000.0f};
    auto s = backend.RoPE(q.view, k.view, 0, config);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code, ErrorCode::kInvalidArgument);
}

TEST(CpuBackendTest, AttentionNullKV) {
    auto q = HostTensor::alloc({1, 2, 4}, DType::kF32);
    auto out = HostTensor::alloc({1, 8}, DType::kF32);
    KVCacheView kv{.keys = nullptr,
                   .values = nullptr,
                   .seq_len = 1,
                   .num_kv_heads = 1,
                   .head_dim = 4,
                   .dtype = DType::kF32};
    AttentionConfig config{.num_heads = 2, .num_kv_heads = 1, .head_dim = 4};
    CpuBackend backend;
    auto s = backend.Attention(out.view, q.view, kv, config);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code, ErrorCode::kInvalidArgument);
}

TEST(CpuBackendTest, AddInPlaceShapeMismatch) {
    auto x = HostTensor::alloc({1, 4}, DType::kF32);
    auto r = HostTensor::alloc({1, 8}, DType::kF32);
    CpuBackend backend;
    auto s = backend.AddInPlace(x.view, r.view);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code, ErrorCode::kInvalidArgument);
}

// AddBiasInPlace (Qwen2 QKV bias)

TEST(CpuBackendTest, AddBiasInPlaceF32) {
    auto x = HostTensor::alloc({2, 4}, DType::kF32);
    auto bias = HostTensor::alloc({4}, DType::kF32);
    float* xp = x.view.data_as<float>();
    float* bp = bias.view.data_as<float>();
    for (int i = 0; i < 8; ++i) {
        xp[i] = static_cast<float>(i);
    }
    for (int i = 0; i < 4; ++i) {
        bp[i] = 0.5f * static_cast<float>(i);
    }

    CpuBackend backend;
    ASSERT_TRUE(backend.AddBiasInPlace(x.view, bias.view).ok());

    for (int b = 0; b < 2; ++b) {
        for (int i = 0; i < 4; ++i) {
            EXPECT_FLOAT_EQ(xp[b * 4 + i], static_cast<float>(b * 4 + i) + bp[i]);
        }
    }
}

TEST(CpuBackendTest, AddBiasInPlaceF16Bias) {
    // bias may arrive as f16 (typical for Qwen2 GGUF); it must be converted.
    auto x = HostTensor::alloc({1, 4}, DType::kF32);
    auto bias = HostTensor::alloc({4}, DType::kF16);
    float* xp = x.view.data_as<float>();
    for (int i = 0; i < 4; ++i) {
        xp[i] = 1.0f;
        bias.view.data_as<uint16_t>()[i] = fp32_to_fp16(0.25f * static_cast<float>(i));
    }

    CpuBackend backend;
    ASSERT_TRUE(backend.AddBiasInPlace(x.view, bias.view).ok());
    for (int i = 0; i < 4; ++i) {
        EXPECT_NEAR(xp[i], 1.0f + 0.25f * static_cast<float>(i), 1e-3f);
    }
}

TEST(CpuBackendTest, AddBiasInPlaceShapeMismatch) {
    auto x = HostTensor::alloc({1, 4}, DType::kF32);
    auto bias = HostTensor::alloc({8}, DType::kF32);
    CpuBackend backend;
    auto s = backend.AddBiasInPlace(x.view, bias.view);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code, ErrorCode::kInvalidArgument);
}

} // namespace
} // namespace pl::mllm
