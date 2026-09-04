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

// Q4_0 block (ggml layout): fp16 scale + 16 packed nibbles; low nibbles
// map to elements [0,16), high nibbles to [16,32).
struct Q4Block {
    uint16_t scale;
    uint8_t qs[16];
};
static_assert(sizeof(Q4Block) == kQ4_0TypeSize);

// Build a Q4_0 weight buffer from f32 data (block-aligned in_dim).
void fill_q4_0(HostTensor& w, const std::vector<float>& w_f32) {
    auto* blocks = static_cast<Q4Block*>(w.buf.data());
    const int64_t numel = w.view.shape().numel();
    const int64_t num_blocks = numel / kQ4_0BlockSize;
    for (int64_t blk = 0; blk < num_blocks; ++blk) {
        const float* src = w_f32.data() + blk * 32;
        float max_abs = 0.0f;
        for (int j = 0; j < 32; ++j) {
            max_abs = std::max(max_abs, std::abs(src[j]));
        }
        const float scale = max_abs >= 1e-8f ? max_abs / -8.0f : 1.0f;
        Q4Block& b = blocks[static_cast<size_t>(blk)];
        b.scale = fp32_to_fp16(scale);
        for (int j = 0; j < 16; ++j) {
            const int q0 = std::max(-8, std::min(7, static_cast<int>(std::round(src[j] / scale))));
            const int q1 =
                std::max(-8, std::min(7, static_cast<int>(std::round(src[j + 16] / scale))));
            b.qs[j] = static_cast<uint8_t>((q0 + 8) | ((q1 + 8) << 4));
        }
    }
}

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

// MatMul parity at real-model dimensions (Qwen3-0.6B-like: hidden=1024,
// intermediate=3072, vocab=151936). Stresses the split-K GEMV with large
// output dimensions and validates accumulation correctness at scale.

TEST(MetalParityTest, MatMulF32WeightRealDims) {
    REQUIRE_METAL();
    const int batch = 1, in_dim = 1024, out_dim = 1024;
    auto x = HostTensor::alloc({batch, in_dim}, DType::kF32);
    auto w = HostTensor::alloc({out_dim, in_dim}, DType::kF32);
    auto out_cpu = HostTensor::alloc({batch, out_dim}, DType::kF32);
    auto out_gpu = HostTensor::alloc({batch, out_dim}, DType::kF32);
    fill_random(x.view.data_as<float>(), batch * in_dim, 31);
    fill_random(w.view.data_as<float>(), out_dim * in_dim, 32);

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

    expect_close(out_cpu.view, out_gpu.view, 1e-3f, 1e-3f);
}

TEST(MetalParityTest, MatMulF16WeightRealDims) {
    REQUIRE_METAL();
    const int batch = 1, in_dim = 1024, out_dim = 1024;
    auto x = HostTensor::alloc({batch, in_dim}, DType::kF32);
    auto w = HostTensor::alloc({out_dim, in_dim}, DType::kF16);
    auto out_cpu = HostTensor::alloc({batch, out_dim}, DType::kF32);
    auto out_gpu = HostTensor::alloc({batch, out_dim}, DType::kF32);
    fill_random(x.view.data_as<float>(), batch * in_dim, 33);

    std::vector<float> w_f32(static_cast<size_t>(out_dim) * in_dim);
    fill_random(w_f32.data(), w_f32.size(), 34);
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

    expect_close(out_cpu.view, out_gpu.view, 1e-2f, 1e-2f);
}

TEST(MetalParityTest, MatMulQ4_0Weight) {
    REQUIRE_METAL();
    const int batch = 1, in_dim = 64, out_dim = 8;
    auto x = HostTensor::alloc({batch, in_dim}, DType::kF32);
    auto w = HostTensor::alloc_q4_0({out_dim, in_dim});
    auto out_cpu = HostTensor::alloc({batch, out_dim}, DType::kF32);
    auto out_gpu = HostTensor::alloc({batch, out_dim}, DType::kF32);
    fill_random(x.view.data_as<float>(), batch * in_dim, 101);

    std::vector<float> w_f32(static_cast<size_t>(out_dim) * in_dim);
    fill_random(w_f32.data(), w_f32.size(), 102);
    fill_q4_0(w, w_f32);

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

    // Both dequantize Q4_0 identically before the MAC; parity is exact
    // modulo f32 accumulation order.
    expect_close(out_cpu.view, out_gpu.view, 1e-3f, 1e-3f);
}

TEST(MetalParityTest, MatMulQ8_0WeightRealDims) {
    REQUIRE_METAL();
    const int batch = 1, in_dim = 1024, out_dim = 1024;
    auto x = HostTensor::alloc({batch, in_dim}, DType::kF32);
    auto w = HostTensor::alloc_q8_0({out_dim, in_dim});
    auto out_cpu = HostTensor::alloc({batch, out_dim}, DType::kF32);
    auto out_gpu = HostTensor::alloc({batch, out_dim}, DType::kF32);
    fill_random(x.view.data_as<float>(), batch * in_dim, 35);

    std::vector<float> w_f32(static_cast<size_t>(out_dim) * in_dim);
    fill_random(w_f32.data(), w_f32.size(), 36);
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

    expect_close(out_cpu.view, out_gpu.view, 1.0f, 1e-2f);
}

// Fused MatMul parity: 3 weights (Q/K/V pattern), f32

TEST(MetalParityTest, MatMulFused3F32) {
    REQUIRE_METAL();
    const int batch = 1, in_dim = 1024;
    const int out0 = 1024, out1 = 256, out2 = 256; // Q/K/V dims
    auto x = HostTensor::alloc({batch, in_dim}, DType::kF32);
    auto w0 = HostTensor::alloc({out0, in_dim}, DType::kF32);
    auto w1 = HostTensor::alloc({out1, in_dim}, DType::kF32);
    auto w2 = HostTensor::alloc({out2, in_dim}, DType::kF32);
    auto o0_cpu = HostTensor::alloc({batch, out0}, DType::kF32);
    auto o1_cpu = HostTensor::alloc({batch, out1}, DType::kF32);
    auto o2_cpu = HostTensor::alloc({batch, out2}, DType::kF32);
    auto o0_gpu = HostTensor::alloc({batch, out0}, DType::kF32);
    auto o1_gpu = HostTensor::alloc({batch, out1}, DType::kF32);
    auto o2_gpu = HostTensor::alloc({batch, out2}, DType::kF32);
    fill_random(x.view.data_as<float>(), batch * in_dim, 41);
    fill_random(w0.view.data_as<float>(), out0 * in_dim, 42);
    fill_random(w1.view.data_as<float>(), out1 * in_dim, 43);
    fill_random(w2.view.data_as<float>(), out2 * in_dim, 44);

    CpuBackend cpu;
    std::array names_cpu = {std::string_view{"w0"}, std::string_view{"w1"}, std::string_view{"w2"}};
    std::array<TensorView, 3> views_cpu = {w0.view, w1.view, w2.view};
    ASSERT_TRUE(cpu.ImportWeights(views_cpu, names_cpu).ok());
    ASSERT_TRUE(cpu.MatMul(o0_cpu.view, x.view, "w0").ok());
    ASSERT_TRUE(cpu.MatMul(o1_cpu.view, x.view, "w1").ok());
    ASSERT_TRUE(cpu.MatMul(o2_cpu.view, x.view, "w2").ok());

    MetalBackend gpu;
    ASSERT_TRUE(gpu.ImportWeights(views_cpu, names_cpu).ok());
    std::array<TensorView, 3> fused_outs = {o0_gpu.view, o1_gpu.view, o2_gpu.view};
    ASSERT_TRUE(gpu.MatMulFused(fused_outs, x.view, names_cpu).ok());
    ASSERT_TRUE(gpu.SyncToHost(o0_gpu.view).ok());
    ASSERT_TRUE(gpu.SyncToHost(o1_gpu.view).ok());
    ASSERT_TRUE(gpu.SyncToHost(o2_gpu.view).ok());

    expect_close(o0_cpu.view, o0_gpu.view, 1e-3f, 1e-3f);
    expect_close(o1_cpu.view, o1_gpu.view, 1e-3f, 1e-3f);
    expect_close(o2_cpu.view, o2_gpu.view, 1e-3f, 1e-3f);
}

// Fused MatMul parity: 2 weights (gate/up pattern), f16

TEST(MetalParityTest, MatMulFused2F16) {
    REQUIRE_METAL();
    const int batch = 1, in_dim = 1024;
    const int out0 = 3072, out1 = 3072; // gate/up
    auto x = HostTensor::alloc({batch, in_dim}, DType::kF32);
    auto w0 = HostTensor::alloc({out0, in_dim}, DType::kF16);
    auto w1 = HostTensor::alloc({out1, in_dim}, DType::kF16);
    auto o0_cpu = HostTensor::alloc({batch, out0}, DType::kF32);
    auto o1_cpu = HostTensor::alloc({batch, out1}, DType::kF32);
    auto o0_gpu = HostTensor::alloc({batch, out0}, DType::kF32);
    auto o1_gpu = HostTensor::alloc({batch, out1}, DType::kF32);
    fill_random(x.view.data_as<float>(), batch * in_dim, 51);

    std::vector<float> w0_f32(static_cast<size_t>(out0) * in_dim);
    std::vector<float> w1_f32(static_cast<size_t>(out1) * in_dim);
    fill_random(w0_f32.data(), w0_f32.size(), 52);
    fill_random(w1_f32.data(), w1_f32.size(), 53);
    for (size_t i = 0; i < w0_f32.size(); ++i)
        w0.view.data_as<uint16_t>()[i] = fp32_to_fp16(w0_f32[i]);
    for (size_t i = 0; i < w1_f32.size(); ++i)
        w1.view.data_as<uint16_t>()[i] = fp32_to_fp16(w1_f32[i]);

    CpuBackend cpu;
    std::array names = {std::string_view{"w0"}, std::string_view{"w1"}};
    std::array<TensorView, 2> views = {w0.view, w1.view};
    ASSERT_TRUE(cpu.ImportWeights(views, names).ok());
    ASSERT_TRUE(cpu.MatMul(o0_cpu.view, x.view, "w0").ok());
    ASSERT_TRUE(cpu.MatMul(o1_cpu.view, x.view, "w1").ok());

    MetalBackend gpu;
    ASSERT_TRUE(gpu.ImportWeights(views, names).ok());
    std::array<TensorView, 2> fused_outs = {o0_gpu.view, o1_gpu.view};
    ASSERT_TRUE(gpu.MatMulFused(fused_outs, x.view, names).ok());
    ASSERT_TRUE(gpu.SyncToHost(o0_gpu.view).ok());
    ASSERT_TRUE(gpu.SyncToHost(o1_gpu.view).ok());

    expect_close(o0_cpu.view, o0_gpu.view, 1e-2f, 1e-2f);
    expect_close(o1_cpu.view, o1_gpu.view, 1e-2f, 1e-2f);
}

TEST(MetalParityTest, MatMulQ4_0WeightRealDims) {
    REQUIRE_METAL();
    const int batch = 1, in_dim = 1024, out_dim = 1024;
    auto x = HostTensor::alloc({batch, in_dim}, DType::kF32);
    auto w = HostTensor::alloc_q4_0({out_dim, in_dim});
    auto out_cpu = HostTensor::alloc({batch, out_dim}, DType::kF32);
    auto out_gpu = HostTensor::alloc({batch, out_dim}, DType::kF32);
    fill_random(x.view.data_as<float>(), batch * in_dim, 111);

    std::vector<float> w_f32(static_cast<size_t>(out_dim) * in_dim);
    fill_random(w_f32.data(), w_f32.size(), 112);
    fill_q4_0(w, w_f32);

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

    expect_close(out_cpu.view, out_gpu.view, 1.0f, 1e-2f);
}

// Fused MatMul parity: 3 weights, Q8_0

TEST(MetalParityTest, MatMulFused3Q8_0) {
    REQUIRE_METAL();
    const int batch = 1, in_dim = 1024;
    const int out0 = 1024, out1 = 256, out2 = 256;
    auto x = HostTensor::alloc({batch, in_dim}, DType::kF32);
    auto w0 = HostTensor::alloc_q8_0({out0, in_dim});
    auto w1 = HostTensor::alloc_q8_0({out1, in_dim});
    auto w2 = HostTensor::alloc_q8_0({out2, in_dim});
    auto o0_cpu = HostTensor::alloc({batch, out0}, DType::kF32);
    auto o1_cpu = HostTensor::alloc({batch, out1}, DType::kF32);
    auto o2_cpu = HostTensor::alloc({batch, out2}, DType::kF32);
    auto o0_gpu = HostTensor::alloc({batch, out0}, DType::kF32);
    auto o1_gpu = HostTensor::alloc({batch, out1}, DType::kF32);
    auto o2_gpu = HostTensor::alloc({batch, out2}, DType::kF32);
    fill_random(x.view.data_as<float>(), batch * in_dim, 61);

    std::vector<float> w0_f32(static_cast<size_t>(out0) * in_dim);
    std::vector<float> w1_f32(static_cast<size_t>(out1) * in_dim);
    std::vector<float> w2_f32(static_cast<size_t>(out2) * in_dim);
    fill_random(w0_f32.data(), w0_f32.size(), 62);
    fill_random(w1_f32.data(), w1_f32.size(), 63);
    fill_random(w2_f32.data(), w2_f32.size(), 64);
    fill_q8_0(w0, w0_f32);
    fill_q8_0(w1, w1_f32);
    fill_q8_0(w2, w2_f32);

    CpuBackend cpu;
    std::array names = {std::string_view{"w0"}, std::string_view{"w1"}, std::string_view{"w2"}};
    std::array<TensorView, 3> views = {w0.view, w1.view, w2.view};
    ASSERT_TRUE(cpu.ImportWeights(views, names).ok());
    ASSERT_TRUE(cpu.MatMul(o0_cpu.view, x.view, "w0").ok());
    ASSERT_TRUE(cpu.MatMul(o1_cpu.view, x.view, "w1").ok());
    ASSERT_TRUE(cpu.MatMul(o2_cpu.view, x.view, "w2").ok());

    MetalBackend gpu;
    ASSERT_TRUE(gpu.ImportWeights(views, names).ok());
    std::array<TensorView, 3> fused_outs = {o0_gpu.view, o1_gpu.view, o2_gpu.view};
    ASSERT_TRUE(gpu.MatMulFused(fused_outs, x.view, names).ok());
    ASSERT_TRUE(gpu.SyncToHost(o0_gpu.view).ok());
    ASSERT_TRUE(gpu.SyncToHost(o1_gpu.view).ok());
    ASSERT_TRUE(gpu.SyncToHost(o2_gpu.view).ok());

    expect_close(o0_cpu.view, o0_gpu.view, 1.0f, 1e-2f);
    expect_close(o1_cpu.view, o1_gpu.view, 1.0f, 1e-2f);
    expect_close(o2_cpu.view, o2_gpu.view, 1.0f, 1e-2f);
}

TEST(MetalParityTest, MatMulFused3Q4_0) {
    REQUIRE_METAL();
    const int batch = 1, in_dim = 1024;
    const int out0 = 1024, out1 = 256, out2 = 256;
    auto x = HostTensor::alloc({batch, in_dim}, DType::kF32);
    auto w0 = HostTensor::alloc_q4_0({out0, in_dim});
    auto w1 = HostTensor::alloc_q4_0({out1, in_dim});
    auto w2 = HostTensor::alloc_q4_0({out2, in_dim});
    auto o0_cpu = HostTensor::alloc({batch, out0}, DType::kF32);
    auto o1_cpu = HostTensor::alloc({batch, out1}, DType::kF32);
    auto o2_cpu = HostTensor::alloc({batch, out2}, DType::kF32);
    auto o0_gpu = HostTensor::alloc({batch, out0}, DType::kF32);
    auto o1_gpu = HostTensor::alloc({batch, out1}, DType::kF32);
    auto o2_gpu = HostTensor::alloc({batch, out2}, DType::kF32);
    fill_random(x.view.data_as<float>(), batch * in_dim, 121);

    std::vector<float> w0_f32(static_cast<size_t>(out0) * in_dim);
    std::vector<float> w1_f32(static_cast<size_t>(out1) * in_dim);
    std::vector<float> w2_f32(static_cast<size_t>(out2) * in_dim);
    fill_random(w0_f32.data(), w0_f32.size(), 122);
    fill_random(w1_f32.data(), w1_f32.size(), 123);
    fill_random(w2_f32.data(), w2_f32.size(), 124);
    fill_q4_0(w0, w0_f32);
    fill_q4_0(w1, w1_f32);
    fill_q4_0(w2, w2_f32);

    CpuBackend cpu;
    std::array names = {std::string_view{"w0"}, std::string_view{"w1"}, std::string_view{"w2"}};
    std::array<TensorView, 3> views = {w0.view, w1.view, w2.view};
    ASSERT_TRUE(cpu.ImportWeights(views, names).ok());
    ASSERT_TRUE(cpu.MatMul(o0_cpu.view, x.view, "w0").ok());
    ASSERT_TRUE(cpu.MatMul(o1_cpu.view, x.view, "w1").ok());
    ASSERT_TRUE(cpu.MatMul(o2_cpu.view, x.view, "w2").ok());

    MetalBackend gpu;
    ASSERT_TRUE(gpu.ImportWeights(views, names).ok());
    std::array<TensorView, 3> fused_outs = {o0_gpu.view, o1_gpu.view, o2_gpu.view};
    ASSERT_TRUE(gpu.MatMulFused(fused_outs, x.view, names).ok());
    ASSERT_TRUE(gpu.SyncToHost(o0_gpu.view).ok());
    ASSERT_TRUE(gpu.SyncToHost(o1_gpu.view).ok());
    ASSERT_TRUE(gpu.SyncToHost(o2_gpu.view).ok());

    expect_close(o0_cpu.view, o0_gpu.view, 1.0f, 1e-2f);
    expect_close(o1_cpu.view, o1_gpu.view, 1.0f, 1e-2f);
    expect_close(o2_cpu.view, o2_gpu.view, 1.0f, 1e-2f);
}

// RmsNorm parity

// Batched (prefill) GEMM parity: batch > 1 routes through the Metal backend's
// MPS path with a lazily dequantized f16 weight cache and f16 GEMM
// operands.  f16 prefill is the industry standard (llama.cpp mul_mm, vLLM
// bf16) and the end-to-end byte-exact gate against llama.cpp holds; the
// tolerance therefore reflects f16 operand rounding (~1e-3 relative per
// element, RSS over the dot product) rather than f32.
// Layout/pitch bugs produce O(1) errors and are still caught.
TEST(MetalParityTest, MatMulQ8_0BatchPrefill) {
    REQUIRE_METAL();
    const int batch = 4, in_dim = 256, out_dim = 32;
    auto x = HostTensor::alloc({batch, in_dim}, DType::kF32);
    auto w = HostTensor::alloc_q8_0({out_dim, in_dim});
    auto out_cpu = HostTensor::alloc({batch, out_dim}, DType::kF32);
    auto out_gpu = HostTensor::alloc({batch, out_dim}, DType::kF32);
    fill_random(x.view.data_as<float>(), batch * in_dim, 51);

    std::vector<float> w_f32(static_cast<size_t>(out_dim) * in_dim);
    fill_random(w_f32.data(), w_f32.size(), 52);
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

    expect_close(out_cpu.view, out_gpu.view, 1e-2f, 1e-2f);
}

// Batched f16-weight GEMM parity: f16 weights bind directly into the f16
// MPS GEMM (no dequant copy).
TEST(MetalParityTest, MatMulF16BatchPrefill) {
    REQUIRE_METAL();
    const int batch = 4, in_dim = 256, out_dim = 32;
    auto x = HostTensor::alloc({batch, in_dim}, DType::kF32);
    auto w = HostTensor::alloc({out_dim, in_dim}, DType::kF16);
    auto out_cpu = HostTensor::alloc({batch, out_dim}, DType::kF32);
    auto out_gpu = HostTensor::alloc({batch, out_dim}, DType::kF32);
    fill_random(x.view.data_as<float>(), batch * in_dim, 81);

    std::vector<float> w_f32(static_cast<size_t>(out_dim) * in_dim);
    fill_random(w_f32.data(), w_f32.size(), 82);
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

    expect_close(out_cpu.view, out_gpu.view, 1e-2f, 1e-2f);
}

// Same as MatMulQ8_0BatchPrefill but at real model dimensions
// (Qwen3-0.6B q_proj: 15 prompt tokens, in=1024, out=2048).
TEST(MetalParityTest, MatMulQ8_0BatchPrefillRealDims) {
    REQUIRE_METAL();
    const int batch = 15, in_dim = 1024, out_dim = 2048;
    auto x = HostTensor::alloc({batch, in_dim}, DType::kF32);
    auto w = HostTensor::alloc_q8_0({out_dim, in_dim});
    auto out_cpu = HostTensor::alloc({batch, out_dim}, DType::kF32);
    auto out_gpu = HostTensor::alloc({batch, out_dim}, DType::kF32);
    fill_random(x.view.data_as<float>(), batch * in_dim, 71);

    std::vector<float> w_f32(static_cast<size_t>(out_dim) * in_dim);
    fill_random(w_f32.data(), w_f32.size(), 72);
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

    expect_close(out_cpu.view, out_gpu.view, 1e-2f, 1e-2f);
}

// Batched Q4_0 GEMM parity: the prefill path lazily dequantizes the
// weight to f16 and runs the f16 MPS GEMM.
TEST(MetalParityTest, MatMulQ4_0BatchPrefill) {
    REQUIRE_METAL();
    const int batch = 4, in_dim = 256, out_dim = 32;
    auto x = HostTensor::alloc({batch, in_dim}, DType::kF32);
    auto w = HostTensor::alloc_q4_0({out_dim, in_dim});
    auto out_cpu = HostTensor::alloc({batch, out_dim}, DType::kF32);
    auto out_gpu = HostTensor::alloc({batch, out_dim}, DType::kF32);
    fill_random(x.view.data_as<float>(), batch * in_dim, 131);

    std::vector<float> w_f32(static_cast<size_t>(out_dim) * in_dim);
    fill_random(w_f32.data(), w_f32.size(), 132);
    fill_q4_0(w, w_f32);

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

    expect_close(out_cpu.view, out_gpu.view, 1e-2f, 1e-2f);
}

// Batched RoPE parity: row b must be rotated at position + b.
TEST(MetalParityTest, RoPEBatchPrefillParity) {
    REQUIRE_METAL();
    const int batch = 3, num_heads = 8, num_kv_heads = 4, head_dim = 64;
    auto q_cpu = HostTensor::alloc({batch, num_heads, head_dim}, DType::kF32);
    auto k_cpu = HostTensor::alloc({batch, num_kv_heads, head_dim}, DType::kF32);
    auto q_gpu = HostTensor::alloc({batch, num_heads, head_dim}, DType::kF32);
    auto k_gpu = HostTensor::alloc({batch, num_kv_heads, head_dim}, DType::kF32);
    const size_t q_n = static_cast<size_t>(q_cpu.view.shape().numel());
    const size_t k_n = static_cast<size_t>(k_cpu.view.shape().numel());
    fill_random(q_cpu.view.data_as<float>(), q_n, 61);
    fill_random(k_cpu.view.data_as<float>(), k_n, 62);
    std::memcpy(q_gpu.view.data(), q_cpu.view.data(), q_n * sizeof(float));
    std::memcpy(k_gpu.view.data(), k_cpu.view.data(), k_n * sizeof(float));

    RopeConfig cfg{.head_dim = head_dim, .freq_base = 1000000.0f};
    CpuBackend cpu;
    ASSERT_TRUE(cpu.RoPE(q_cpu.view, k_cpu.view, 5, cfg).ok());
    MetalBackend gpu;
    ASSERT_TRUE(gpu.RoPE(q_gpu.view, k_gpu.view, 5, cfg).ok());
    ASSERT_TRUE(gpu.SyncToHost(q_gpu.view).ok());
    ASSERT_TRUE(gpu.SyncToHost(k_gpu.view).ok());

    expect_close(q_cpu.view, q_gpu.view, 2e-2f, 2e-2f);
    expect_close(k_cpu.view, k_gpu.view, 2e-2f, 2e-2f);
}

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

// Batched prefill attention parity: batched AppendKV + AttentionPrefillKV
// must equal per-row causal CPU attention, including positions before the
// batch (causal prefix continuity).
TEST(MetalParityTest, DeviceKVPrefillAttentionParity) {
    REQUIRE_METAL();
    const int num_heads = 4, num_kv_heads = 2, head_dim = 8;
    const int start_pos = 2, n = 3, capacity = 8;

    // Prior context in the KV cache: positions [0, start_pos).
    const int hist = start_pos;
    auto hist_k = HostTensor::alloc({hist, num_kv_heads, head_dim}, DType::kF32);
    auto hist_v = HostTensor::alloc({hist, num_kv_heads, head_dim}, DType::kF32);
    auto k_rows = HostTensor::alloc({n, num_kv_heads, head_dim}, DType::kF32);
    auto v_rows = HostTensor::alloc({n, num_kv_heads, head_dim}, DType::kF32);
    auto q_rows = HostTensor::alloc({n, num_heads, head_dim}, DType::kF32);
    auto out_gpu = HostTensor::alloc({n, num_heads * head_dim}, DType::kF32);
    fill_random(hist_k.view.data_as<float>(), hist * num_kv_heads * head_dim, 21);
    fill_random(hist_v.view.data_as<float>(), hist * num_kv_heads * head_dim, 22);
    fill_random(k_rows.view.data_as<float>(), n * num_kv_heads * head_dim, 23);
    fill_random(v_rows.view.data_as<float>(), n * num_kv_heads * head_dim, 24);
    fill_random(q_rows.view.data_as<float>(), n * num_heads * head_dim, 25);

    // Full combined KV [start_pos + n, kv, hd] for the CPU reference.
    const int total = start_pos + n;
    std::vector<float> all_k(static_cast<size_t>(total) * num_kv_heads * head_dim);
    std::vector<float> all_v(static_cast<size_t>(total) * num_kv_heads * head_dim);
    const size_t row_bytes = static_cast<size_t>(num_kv_heads) * head_dim * sizeof(float);
    std::memcpy(all_k.data(), hist_k.view.data(), static_cast<size_t>(hist) * row_bytes);
    std::memcpy(all_v.data(), hist_v.view.data(), static_cast<size_t>(hist) * row_bytes);
    std::memcpy(all_k.data() + static_cast<size_t>(hist) * num_kv_heads * head_dim,
                k_rows.view.data(),
                static_cast<size_t>(n) * row_bytes);
    std::memcpy(all_v.data() + static_cast<size_t>(hist) * num_kv_heads * head_dim,
                v_rows.view.data(),
                static_cast<size_t>(n) * row_bytes);

    AttentionConfig cfg{
        .num_heads = num_heads,
        .num_kv_heads = num_kv_heads,
        .head_dim = head_dim,
        .scale = 1.0f / std::sqrt(static_cast<float>(head_dim)),
    };

    // Metal: device-KV path with batched appends and batched attention.
    MetalBackend gpu;
    ASSERT_TRUE(gpu.ConfigureDeviceKV(1, num_kv_heads, head_dim, capacity).ok());
    ASSERT_TRUE(gpu.AppendKV(0, hist_k.view, hist_v.view, 0).ok());
    ASSERT_TRUE(gpu.AppendKV(0, k_rows.view, v_rows.view, start_pos).ok());
    ASSERT_TRUE(gpu.AttentionPrefillKV(out_gpu.view, q_rows.view, 0, start_pos + 1, cfg).ok());
    ASSERT_TRUE(gpu.SyncToHost(out_gpu.view).ok());

    // CPU reference per row: causal prefix [0, start_pos + i].
    CpuBackend cpu;
    for (int i = 0; i < n; ++i) {
        const int row_seq = start_pos + i + 1;
        KVCacheView kv_host{
            .keys = all_k.data(),
            .values = all_v.data(),
            .seq_len = row_seq,
            .num_kv_heads = num_kv_heads,
            .head_dim = head_dim,
            .dtype = DType::kF32,
        };
        TensorView q_row(static_cast<char*>(q_rows.view.data()) +
                             static_cast<size_t>(i) * num_heads * head_dim * sizeof(float),
                         DType::kF32,
                         Shape({1, num_heads, head_dim}));
        auto out_ref = HostTensor::alloc({1, num_heads * head_dim}, DType::kF32);
        ASSERT_TRUE(cpu.Attention(out_ref.view, q_row, kv_host, cfg).ok());
        TensorView gpu_row(static_cast<char*>(out_gpu.view.data()) +
                               static_cast<size_t>(i) * num_heads * head_dim * sizeof(float),
                           DType::kF32,
                           Shape({1, num_heads * head_dim}));
        SCOPED_TRACE("row " + std::to_string(i) + " (seq=" + std::to_string(row_seq) + ")");
        expect_close(out_ref.view, gpu_row, 1e-5f, 1e-5f);
    }
}

// Ring-mode device-KV shift parity: AppendKV writes 8 distinct positions,
// ShiftKV(3) compacts them (memmove semantics), and the attention result over
// the remaining physical range must equal CPU attention over the retained
// positions — proving the shift moved exactly the right K/V rows.
TEST(MetalParityTest, DeviceKVShiftParity) {
    REQUIRE_METAL();
    const int num_heads = 2, num_kv_heads = 1, head_dim = 8;
    const int capacity = 8, drop = 3;
    auto q = HostTensor::alloc({1, num_heads, head_dim}, DType::kF32);
    fill_random(q.view.data_as<float>(), num_heads * head_dim, 31);

    // CPU reference: attention over retained positions [drop, capacity).
    auto all_k = HostTensor::alloc({capacity, num_kv_heads, head_dim}, DType::kF32);
    auto all_v = HostTensor::alloc({capacity, num_kv_heads, head_dim}, DType::kF32);
    fill_random(all_k.view.data_as<float>(), capacity * num_kv_heads * head_dim, 32);
    fill_random(all_v.view.data_as<float>(), capacity * num_kv_heads * head_dim, 33);
    KVCacheView kv_host{
        .keys = static_cast<char*>(all_k.view.data()) +
                static_cast<size_t>(drop) * num_kv_heads * head_dim * sizeof(float),
        .values = static_cast<char*>(all_v.view.data()) +
                  static_cast<size_t>(drop) * num_kv_heads * head_dim * sizeof(float),
        .seq_len = capacity - drop,
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
    auto out_cpu = HostTensor::alloc({1, num_heads * head_dim}, DType::kF32);
    CpuBackend cpu;
    ASSERT_TRUE(cpu.Attention(out_cpu.view, q.view, kv_host, cfg).ok());

    // Metal: device KV, full append then ShiftKV(drop).
    MetalBackend gpu;
    ASSERT_TRUE(gpu.ConfigureDeviceKV(1, num_kv_heads, head_dim, capacity).ok());
    for (int s = 0; s < capacity; ++s) {
        TensorView k_slice(
            static_cast<char*>(all_k.view.data()) +
                static_cast<size_t>(s) * num_kv_heads * head_dim * sizeof(float),
            DType::kF32,
            Shape({1, num_kv_heads, head_dim}));
        TensorView v_slice(
            static_cast<char*>(all_v.view.data()) +
                static_cast<size_t>(s) * num_kv_heads * head_dim * sizeof(float),
            DType::kF32,
            Shape({1, num_kv_heads, head_dim}));
        ASSERT_TRUE(gpu.AppendKV(0, k_slice, v_slice, s).ok());
    }
    // Invalid shift sizes are rejected up front.
    EXPECT_FALSE(gpu.ShiftKV(-1).ok());
    EXPECT_FALSE(gpu.ShiftKV(capacity + 1).ok());
    ASSERT_TRUE(gpu.ShiftKV(drop).ok());

    auto out_gpu = HostTensor::alloc({1, num_heads * head_dim}, DType::kF32);
    ASSERT_TRUE(gpu.AttentionKV(out_gpu.view, q.view, 0, capacity - drop, cfg).ok());
    ASSERT_TRUE(gpu.SyncToHost(out_gpu.view).ok());

    expect_close(out_cpu.view, out_gpu.view, 1e-5f, 1e-5f);
}

// Attention parity at real-model dimensions (Qwen3-like):
//   head_dim=128, GQA group=4 (32 q heads, 8 kv heads), seq=128.
//   This stresses the flash-attention cooperative kernel with realistic
//   sizes and validates the online-softmax merging across blocks.

TEST(MetalParityTest, AttentionParityRealDims) {
    REQUIRE_METAL();
    const int num_heads = 32, num_kv_heads = 8, head_dim = 128, seq_len = 128;
    auto q = HostTensor::alloc({1, num_heads, head_dim}, DType::kF32);
    auto keys = HostTensor::alloc({seq_len, num_kv_heads, head_dim}, DType::kF32);
    auto values = HostTensor::alloc({seq_len, num_kv_heads, head_dim}, DType::kF32);
    auto out_cpu = HostTensor::alloc({1, num_heads * head_dim}, DType::kF32);
    auto out_gpu = HostTensor::alloc({1, num_heads * head_dim}, DType::kF32);
    fill_random(q.view.data_as<float>(), num_heads * head_dim, 21);
    fill_random(keys.view.data_as<float>(), seq_len * num_kv_heads * head_dim, 22);
    fill_random(values.view.data_as<float>(), seq_len * num_kv_heads * head_dim, 23);

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

    // Online-softmax accumulation order differs from CPU's two-pass, so use
    // a slightly looser tolerance than the small-dim test.
    expect_close(out_cpu.view, out_gpu.view, 1e-4f, 1e-3f);
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

// Error propagation: a GPU-side failure must surface from Synchronize /
// SyncToHost and turn every subsequent op into an error (fail loud). Before
// the fix, flush() cleared the command buffer BEFORE checking its status, so
// the error check could never fire and device faults were silently dropped.
// Real MTLCommandBuffer errors need hardware faults, so this test injects a
// synthetic error through the test-only seam and verifies the contract.
TEST(MetalBackendErrorTest, GpuErrorIsStickyAndReported) {
    REQUIRE_METAL();
    MetalBackend gpu;
    gpu.InjectGpuErrorForTest(
        Status::Error(ErrorCode::kBackendFailure, "synthetic GPU fault"));

    auto s = gpu.Synchronize();
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.code, ErrorCode::kBackendFailure);
    EXPECT_NE(s.message.find("synthetic GPU fault"), std::string::npos);

    auto x = HostTensor::alloc({1, 16}, DType::kF32);
    auto y = HostTensor::alloc({1, 16}, DType::kF32);
    EXPECT_FALSE(gpu.SyncToHost(x.view).ok());
    EXPECT_FALSE(gpu.AddInPlace(x.view, y.view).ok());
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
