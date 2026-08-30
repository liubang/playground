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


// Metal backend parity tests: run identical inputs through CpuBackend and
// MetalBackend and require near-identical outputs. These are the CPU-reference
// correctness gates required by SPEC §5.2 / §5.3.

#import <Metal/Metal.h>

#include <cmath>
#include <cstring>
#include <random>
#include <vector>

#include "cpp/pl/mllm/backend/cpu/cpu_backend.h"
#include "cpp/pl/mllm/backend/metal/metal_backend.h"
#include "cpp/pl/mllm/core/buffer.h"
#include "cpp/pl/mllm/core/dtype.h"
#include "cpp/pl/mllm/core/tensor.h"
#include "gtest/gtest.h"

namespace pl::mllm {
namespace {

// Test helpers

struct HostTensor {
    OwnedBuffer buf;
    TensorView view;

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
};

// Skip the enclosing test when no Metal device is available (e.g. CI without
// a GPU). Runs before any backend construction.
#define REQUIRE_METAL()                                                 \
    do {                                                                \
        @autoreleasepool {                                              \
            if (MTLCreateSystemDefaultDevice() == nil) {                \
                GTEST_SKIP() << "no Metal device available";            \
            }                                                           \
        }                                                               \
    } while (0)

void fill_random(float* data, size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (size_t i = 0; i < n; ++i) {
        data[i] = dist(rng);
    }
}

void expect_close(const TensorView& cpu, const TensorView& gpu,
                  float abs_tol, float rel_tol) {
    ASSERT_EQ(cpu.shape(), gpu.shape());
    const int64_t n = cpu.shape().numel();
    for (int64_t i = 0; i < n; ++i) {
        const float a = cpu.data_as<float>()[static_cast<size_t>(i)];
        const float b = gpu.data_as<float>()[static_cast<size_t>(i)];
        EXPECT_NEAR(a, b, std::abs(b) * rel_tol + abs_tol)
            << "index " << i;
    }
}

struct Q8Block {
    uint16_t scale;
    int8_t qs[32];
};

// Build a Q8_0 weight buffer from f32 data (block-aligned in_dim).
void fill_q8_0(HostTensor& w, const std::vector<float>& w_f32) {
    auto* blocks = static_cast<Q8Block*>(w.buf.data());
    const int64_t numel = w.view.shape().numel();
    const int64_t num_blocks = numel / kQ8_0BlockSize;
    for (int64_t blk = 0; blk < num_blocks; ++blk) {
        float max_abs = 0.0f;
        for (int j = 0; j < 32; ++j) {
            max_abs = std::max(max_abs, std::abs(w_f32[static_cast<size_t>(blk * 32 + j)]));
        }
        const float scale = max_abs / 127.0f;
        blocks[static_cast<size_t>(blk)].scale = fp32_to_fp16(scale);
        for (int j = 0; j < 32; ++j) {
            const int q = static_cast<int>(std::round(
                w_f32[static_cast<size_t>(blk * 32 + j)] / scale));
            blocks[static_cast<size_t>(blk)].qs[j] =
                static_cast<int8_t>(std::max(-128, std::min(127, q)));
        }
    }
}

// MatMul parity

TEST(MetalParityTest, MatMulF32Weight) {
    REQUIRE_METAL();
    const int batch = 1, in_dim = 16, out_dim = 8;
    auto x = HostTensor::alloc({batch, in_dim}, DType::kF32);
    auto w = HostTensor::alloc({out_dim, in_dim}, DType::kF32);
    auto out_cpu = HostTensor::alloc({batch, out_dim}, DType::kF32);
    auto out_gpu = HostTensor::alloc({batch, out_dim}, DType::kF32);
    fill_random(x.view.data_as<float>(), batch * in_dim, 1);
    fill_random(w.view.data_as<float>(), out_dim * in_dim, 2);

    CpuBackend cpu;
    std::array names_cpu = {std::string_view{"w0"}};
    std::array<TensorView, 1> views_cpu = {w.view};
    ASSERT_TRUE(cpu.ImportWeights(views_cpu, names_cpu).ok());
    ASSERT_TRUE(cpu.MatMul(out_cpu.view, x.view, "w0").ok());

    MetalBackend gpu;
    std::array names_gpu = {std::string_view{"w0"}};
    std::array<TensorView, 1> views_gpu = {w.view};
    ASSERT_TRUE(gpu.ImportWeights(views_gpu, names_gpu).ok());
    ASSERT_TRUE(gpu.MatMul(out_gpu.view, x.view, "w0").ok());
    ASSERT_TRUE(gpu.SyncToHost(out_gpu.view).ok());

    expect_close(out_cpu.view, out_gpu.view, 1e-4f, 1e-4f);
}

TEST(MetalParityTest, MatMulF16Weight) {
    REQUIRE_METAL();
    const int batch = 1, in_dim = 16, out_dim = 8;
    auto x = HostTensor::alloc({batch, in_dim}, DType::kF32);
    auto w = HostTensor::alloc({out_dim, in_dim}, DType::kF16);
    auto out_cpu = HostTensor::alloc({batch, out_dim}, DType::kF32);
    auto out_gpu = HostTensor::alloc({batch, out_dim}, DType::kF32);
    fill_random(x.view.data_as<float>(), batch * in_dim, 3);

    std::vector<float> w_f32(static_cast<size_t>(out_dim) * in_dim);
    fill_random(w_f32.data(), w_f32.size(), 4);
    auto* wh = w.view.data_as<uint16_t>();
    for (size_t i = 0; i < w_f32.size(); ++i) {
        wh[i] = fp32_to_fp16(w_f32[i]);
    }

    CpuBackend cpu;
    std::array names_cpu = {std::string_view{"w0"}};
    std::array<TensorView, 1> views_cpu = {w.view};
    ASSERT_TRUE(cpu.ImportWeights(views_cpu, names_cpu).ok());
    ASSERT_TRUE(cpu.MatMul(out_cpu.view, x.view, "w0").ok());

    MetalBackend gpu;
    std::array names_gpu = {std::string_view{"w0"}};
    std::array<TensorView, 1> views_gpu = {w.view};
    ASSERT_TRUE(gpu.ImportWeights(views_gpu, names_gpu).ok());
    ASSERT_TRUE(gpu.MatMul(out_gpu.view, x.view, "w0").ok());
    ASSERT_TRUE(gpu.SyncToHost(out_gpu.view).ok());

    // MPS f32 math vs CPU: small fp16 weight quantization is identical (both
    // dequantize to f32), so tight tolerance holds.
    expect_close(out_cpu.view, out_gpu.view, 1e-4f, 1e-4f);
}

TEST(MetalParityTest, MatMulQ8_0Weight) {
    REQUIRE_METAL();
    const int batch = 1, in_dim = 32, out_dim = 8;
    auto x = HostTensor::alloc({batch, in_dim}, DType::kF32);
    auto w = HostTensor::alloc_q8_0({out_dim, in_dim});
    auto out_cpu = HostTensor::alloc({batch, out_dim}, DType::kF32);
    auto out_gpu = HostTensor::alloc({batch, out_dim}, DType::kF32);
    fill_random(x.view.data_as<float>(), batch * in_dim, 5);

    std::vector<float> w_f32(static_cast<size_t>(out_dim) * in_dim);
    fill_random(w_f32.data(), w_f32.size(), 6);
    fill_q8_0(w, w_f32);

    CpuBackend cpu;
    std::array names_cpu = {std::string_view{"w0"}};
    std::array<TensorView, 1> views_cpu = {w.view};
    ASSERT_TRUE(cpu.ImportWeights(views_cpu, names_cpu).ok());
    ASSERT_TRUE(cpu.MatMul(out_cpu.view, x.view, "w0").ok());

    MetalBackend gpu;
    std::array names_gpu = {std::string_view{"w0"}};
    std::array<TensorView, 1> views_gpu = {w.view};
    ASSERT_TRUE(gpu.ImportWeights(views_gpu, names_gpu).ok());
    ASSERT_TRUE(gpu.MatMul(out_gpu.view, x.view, "w0").ok());
    ASSERT_TRUE(gpu.SyncToHost(out_gpu.view).ok());

    // Both dequantize Q8_0 identically to f32 before the MAC, so parity is
    // exact modulo f32 rounding in MPS vs scalar CPU.
    expect_close(out_cpu.view, out_gpu.view, 1e-3f, 1e-3f);
}

// RmsNorm parity

TEST(MetalParityTest, RmsNormParity) {
    REQUIRE_METAL();
    const int batch = 2, hidden = 16;
    auto x = HostTensor::alloc({batch, hidden}, DType::kF32);
    auto w = HostTensor::alloc({hidden}, DType::kF32);
    auto out_cpu = HostTensor::alloc({batch, hidden}, DType::kF32);
    auto out_gpu = HostTensor::alloc({batch, hidden}, DType::kF32);
    fill_random(x.view.data_as<float>(), batch * hidden, 7);
    fill_random(w.view.data_as<float>(), hidden, 8);

    CpuBackend cpu;
    ASSERT_TRUE(cpu.RmsNorm(out_cpu.view, x.view, w.view, 1e-5f).ok());

    MetalBackend gpu;
    ASSERT_TRUE(gpu.RmsNorm(out_gpu.view, x.view, w.view, 1e-5f).ok());
    ASSERT_TRUE(gpu.SyncToHost(out_gpu.view).ok());

    expect_close(out_cpu.view, out_gpu.view, 1e-5f, 1e-5f);
}

// RoPE parity

TEST(MetalParityTest, RoPEParity) {
    REQUIRE_METAL();
    const int head_dim = 8, num_heads = 2, num_kv_heads = 1;
    auto q = HostTensor::alloc({1, num_heads, head_dim}, DType::kF32);
    auto k = HostTensor::alloc({1, num_kv_heads, head_dim}, DType::kF32);
    auto q_cpu = HostTensor::alloc({1, num_heads, head_dim}, DType::kF32);
    auto k_cpu = HostTensor::alloc({1, num_kv_heads, head_dim}, DType::kF32);
    fill_random(q.view.data_as<float>(), num_heads * head_dim, 9);
    fill_random(k.view.data_as<float>(), num_kv_heads * head_dim, 10);
    std::memcpy(q_cpu.view.data(), q.view.data(), q.view.byte_size());
    std::memcpy(k_cpu.view.data(), k.view.data(), k.view.byte_size());

    RopeConfig cfg{.head_dim = head_dim, .freq_base = 10000.0f};

    CpuBackend cpu;
    ASSERT_TRUE(cpu.RoPE(q_cpu.view, k_cpu.view, 3, cfg).ok());

    MetalBackend gpu;
    ASSERT_TRUE(gpu.RoPE(q.view, k.view, 3, cfg).ok());
    ASSERT_TRUE(gpu.SyncToHost(q.view).ok());
    ASSERT_TRUE(gpu.SyncToHost(k.view).ok());

    expect_close(q_cpu.view, q.view, 1e-5f, 1e-5f);
    expect_close(k_cpu.view, k.view, 1e-5f, 1e-5f);
}

// Attention parity

TEST(MetalParityTest, AttentionParity) {
    REQUIRE_METAL();
    const int num_heads = 2, num_kv_heads = 1, head_dim = 8, seq_len = 4;
    auto q = HostTensor::alloc({1, num_heads, head_dim}, DType::kF32);
    auto keys = HostTensor::alloc({seq_len, num_kv_heads, head_dim}, DType::kF32);
    auto values = HostTensor::alloc({seq_len, num_kv_heads, head_dim}, DType::kF32);
    auto out_cpu = HostTensor::alloc({1, num_heads * head_dim}, DType::kF32);
    auto out_gpu = HostTensor::alloc({1, num_heads * head_dim}, DType::kF32);
    fill_random(q.view.data_as<float>(), num_heads * head_dim, 11);
    fill_random(keys.view.data_as<float>(), seq_len * num_kv_heads * head_dim, 12);
    fill_random(values.view.data_as<float>(), seq_len * num_kv_heads * head_dim, 13);

    KVCacheView kv{
        .keys = keys.view.data(),
        .values = values.view.data(),
        .seq_len = seq_len,
        .num_kv_heads = num_kv_heads,
        .head_dim = head_dim,
        .dtype = DType::kF32,
    };
    AttentionConfig cfg{
        .num_heads = num_heads,
        .num_kv_heads = num_kv_heads,
        .head_dim = head_dim,
        .scale = 1.0f / std::sqrt(static_cast<float>(head_dim)),
    };

    CpuBackend cpu;
    ASSERT_TRUE(cpu.Attention(out_cpu.view, q.view, kv, cfg).ok());

    MetalBackend gpu;
    ASSERT_TRUE(gpu.Attention(out_gpu.view, q.view, kv, cfg).ok());
    ASSERT_TRUE(gpu.SyncToHost(out_gpu.view).ok());

    expect_close(out_cpu.view, out_gpu.view, 1e-5f, 1e-5f);
}

// Device KV cache parity (AppendKV + AttentionKV)

TEST(MetalParityTest, DeviceKVAttentionParity) {
    REQUIRE_METAL();
    const int num_heads = 2, num_kv_heads = 1, head_dim = 8, seq_len = 4;
    auto q = HostTensor::alloc({1, num_heads, head_dim}, DType::kF32);
    auto keys = HostTensor::alloc({seq_len, num_kv_heads, head_dim}, DType::kF32);
    auto values = HostTensor::alloc({seq_len, num_kv_heads, head_dim}, DType::kF32);
    auto out_cpu = HostTensor::alloc({1, num_heads * head_dim}, DType::kF32);
    auto out_gpu = HostTensor::alloc({1, num_heads * head_dim}, DType::kF32);
    fill_random(q.view.data_as<float>(), num_heads * head_dim, 11);
    fill_random(keys.view.data_as<float>(), seq_len * num_kv_heads * head_dim, 12);
    fill_random(values.view.data_as<float>(), seq_len * num_kv_heads * head_dim, 13);

    // CPU reference: host-KV attention.
    KVCacheView kv_host{
        .keys = keys.view.data(),
        .values = values.view.data(),
        .seq_len = seq_len,
        .num_kv_heads = num_kv_heads,
        .head_dim = head_dim,
        .dtype = DType::kF32,
    };
    AttentionConfig cfg{
        .num_heads = num_heads,
        .num_kv_heads = num_kv_heads,
        .head_dim = head_dim,
        .scale = 1.0f / std::sqrt(static_cast<float>(head_dim)),
    };
    CpuBackend cpu;
    ASSERT_TRUE(cpu.Attention(out_cpu.view, q.view, kv_host, cfg).ok());

    // Metal: device-KV path — configure, append, then AttentionKV.
    MetalBackend gpu;
    ASSERT_TRUE(gpu.ConfigureDeviceKV(1, num_kv_heads, head_dim, seq_len).ok());
    for (int s = 0; s < seq_len; ++s) {
        TensorView k_slice(
            static_cast<char*>(keys.view.data()) +
                static_cast<size_t>(s) * num_kv_heads * head_dim * sizeof(float),
            DType::kF32,
            Shape({1, num_kv_heads, head_dim}));
        TensorView v_slice(
            static_cast<char*>(values.view.data()) +
                static_cast<size_t>(s) * num_kv_heads * head_dim * sizeof(float),
            DType::kF32,
            Shape({1, num_kv_heads, head_dim}));
        ASSERT_TRUE(gpu.AppendKV(0, k_slice, v_slice, s).ok());
    }
    ASSERT_TRUE(gpu.AttentionKV(out_gpu.view, q.view, 0, seq_len, cfg).ok());
    ASSERT_TRUE(gpu.SyncToHost(out_gpu.view).ok());

    expect_close(out_cpu.view, out_gpu.view, 1e-5f, 1e-5f);
}

// SwiGLU / AddInPlace parity

TEST(MetalParityTest, SwiGLUParity) {
    REQUIRE_METAL();
    const int n = 16;
    auto gate = HostTensor::alloc({1, n}, DType::kF32);
    auto up = HostTensor::alloc({1, n}, DType::kF32);
    auto out_cpu = HostTensor::alloc({1, n}, DType::kF32);
    auto out_gpu = HostTensor::alloc({1, n}, DType::kF32);
    fill_random(gate.view.data_as<float>(), n, 14);
    fill_random(up.view.data_as<float>(), n, 15);

    CpuBackend cpu;
    ASSERT_TRUE(cpu.SwiGLU(out_cpu.view, gate.view, up.view).ok());

    MetalBackend gpu;
    ASSERT_TRUE(gpu.SwiGLU(out_gpu.view, gate.view, up.view).ok());
    ASSERT_TRUE(gpu.SyncToHost(out_gpu.view).ok());

    expect_close(out_cpu.view, out_gpu.view, 1e-5f, 1e-5f);
}

TEST(MetalParityTest, AddInPlaceParity) {
    REQUIRE_METAL();
    const int n = 16;
    auto x = HostTensor::alloc({1, n}, DType::kF32);
    auto residual = HostTensor::alloc({1, n}, DType::kF32);
    auto x_cpu = HostTensor::alloc({1, n}, DType::kF32);
    fill_random(x.view.data_as<float>(), n, 16);
    fill_random(residual.view.data_as<float>(), n, 17);
    std::memcpy(x_cpu.view.data(), x.view.data(), x.view.byte_size());

    CpuBackend cpu;
    ASSERT_TRUE(cpu.AddInPlace(x_cpu.view, residual.view).ok());

    MetalBackend gpu;
    ASSERT_TRUE(gpu.AddInPlace(x.view, residual.view).ok());
    ASSERT_TRUE(gpu.SyncToHost(x.view).ok());

    expect_close(x_cpu.view, x.view, 1e-5f, 1e-5f);
}

} // namespace
} // namespace pl::mllm
