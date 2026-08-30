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

// Per-kernel operator benchmarks (SPEC §13.2): RMSNorm across hidden sizes,
// GEMV at target-model dimensions (f32 and fused Q8_0), and Attention across
// sequence lengths. Run with `--backend cpu|metal`.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "cpp/pl/mllm/backend/backend.h"
#include "cpp/pl/mllm/backend/cpu/cpu_backend.h"
#if defined(__APPLE__)
#include "cpp/pl/mllm/backend/metal/metal_backend.h"
#endif
#include "cpp/pl/mllm/bench/bench_common.h"
#include "cpp/pl/mllm/core/buffer.h"
#include "cpp/pl/mllm/core/dtype.h"
#include "cpp/pl/mllm/core/tensor.h"

using namespace pl::mllm;

namespace {

using Clock = std::chrono::steady_clock;

// Simple owning host tensor for benchmark inputs.
struct HostBuf {
    OwnedBuffer buf;
    TensorView view;

    static HostBuf alloc(std::initializer_list<int64_t> dims, DType dtype) {
        Shape shape(dims);
        auto buf = OwnedBuffer::AllocateCpu(dtype_nbytes(dtype, shape.numel()), 64).value();
        TensorView view(buf.data(), dtype, shape);
        return {std::move(buf), view};
    }

    void fill_random(unsigned seed) {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        const int64_t n = view.shape().numel();
        for (int64_t i = 0; i < n; ++i) {
            view.data_as<float>()[static_cast<size_t>(i)] = dist(rng);
        }
    }
};

template <typename Fn> double bench_ms(Fn&& fn, int iters) {
    fn(); // warmup (JIT / first-launch costs)
    fn(); // second warmup (ensures shadow buffers are populated)
    const auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i) {
        fn();
    }
    const auto t1 = Clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count() / static_cast<double>(iters);
}

// For Metal: flush deferred command buffer and wait, so we measure real
// GPU execution time, not just encode time.
void sync_backend(Backend& backend) {
    backend.Synchronize();
}

// RMSNorm

void bench_rmsnorm(Backend& backend, int hidden) {
    auto x = HostBuf::alloc({1, hidden}, DType::kF32);
    auto w = HostBuf::alloc({hidden}, DType::kF32);
    auto out = HostBuf::alloc({1, hidden}, DType::kF32);
    x.fill_random(1);
    w.fill_random(2);

    const double ms = bench_ms(
        [&] {
            auto s = backend.RmsNorm(out.view, x.view, w.view, 1e-5f);
            if (!s.ok())
                std::fprintf(stderr, "rmsnorm failed: %s\n", s.message.c_str());
            sync_backend(backend);
        },
        200);

    const double bytes = 2.0 * static_cast<double>(hidden) * 4.0; // read x+w, write out
    std::printf("rmsnorm   %6d : %9.3f ms  %8.1f GB/s\n", hidden, ms, bytes / (ms / 1e3) / 1e9);
}

// GEMV (MatMul)

// The CPU reference backend stores *non-owning* weight views and ImportWeights
// uses emplace (duplicate names are ignored). Bench weights therefore must
// outlive the backend and each import needs a unique name — keep them in a
// process-lifetime pool.
namespace {

struct BenchWeight {
    HostBuf buf;
    std::string name;
};

} // namespace

void bench_gemv_f32(Backend& backend, int out_dim, int in_dim) {
    static std::vector<BenchWeight> pool;
    auto& w = pool.emplace_back(HostBuf::alloc({out_dim, in_dim}, DType::kF32),
                                "w" + std::to_string(pool.size()));
    auto x = HostBuf::alloc({1, in_dim}, DType::kF32);
    auto out = HostBuf::alloc({1, out_dim}, DType::kF32);
    x.fill_random(1);
    w.buf.fill_random(2);

    std::array names = {std::string_view{w.name}};
    std::array<TensorView, 1> views = {w.buf.view};
    if (!backend.ImportWeights(views, names).ok()) {
        std::fprintf(stderr, "gemv: import failed\n");
        return;
    }

    const double ms = bench_ms(
        [&] {
            auto s = backend.MatMul(out.view, x.view, w.name);
            if (!s.ok())
                std::fprintf(stderr, "gemv failed: %s\n", s.message.c_str());
            sync_backend(backend);
        },
        50);

    const double bytes = (static_cast<double>(out_dim) * in_dim + in_dim + out_dim) * 4.0;
    std::printf("gemv f32  %5d x %-6d : %9.3f ms  %8.1f GB/s\n",
                out_dim,
                in_dim,
                ms,
                bytes / (ms / 1e3) / 1e9);
}

void bench_gemv_q8_0(Backend& backend, int out_dim, int in_dim) {
    static std::vector<BenchWeight> pool;
    auto& w = pool.emplace_back(HostBuf::alloc({out_dim, in_dim}, DType::kQ8_0),
                                "w" + std::to_string(pool.size()));
    auto x = HostBuf::alloc({1, in_dim}, DType::kF32);
    auto out = HostBuf::alloc({1, out_dim}, DType::kF32);
    x.fill_random(1);

    // Fill with a deterministic quantized pattern.
    const int64_t num_blocks = in_dim / kQ8_0BlockSize;
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (int64_t blk = 0; blk < num_blocks * out_dim; ++blk) {
        float max_abs = 0.0f;
        float vals[32];
        for (int j = 0; j < 32; ++j) {
            vals[j] = dist(rng);
            max_abs = std::max(max_abs, std::abs(vals[j]));
        }
        const float scale = max_abs / 127.0f;
        auto* bytes = static_cast<uint8_t*>(w.buf.view.data()) + blk * 34;
        const uint16_t scale_bits = fp32_to_fp16(scale);
        std::memcpy(bytes, &scale_bits, 2);
        for (int j = 0; j < 32; ++j) {
            const int q = static_cast<int>(std::round(vals[j] / scale));
            bytes[2 + j] =
                static_cast<uint8_t>(static_cast<int8_t>(std::max(-128, std::min(127, q))));
        }
    }

    std::array names = {std::string_view{w.name}};
    std::array<TensorView, 1> views = {w.buf.view};
    if (!backend.ImportWeights(views, names).ok()) {
        std::fprintf(stderr, "gemv q8_0: import failed\n");
        return;
    }

    const double ms = bench_ms(
        [&] {
            auto s = backend.MatMul(out.view, x.view, w.name);
            if (!s.ok())
                std::fprintf(stderr, "gemv q8_0 failed: %s\n", s.message.c_str());
            sync_backend(backend);
        },
        50);

    const double bytes = (static_cast<double>(out_dim) * in_dim + in_dim + out_dim) * 4.0;
    std::printf("gemv q8_0 %5d x %-6d : %9.3f ms  %8.1f GB/s\n",
                out_dim,
                in_dim,
                ms,
                bytes / (ms / 1e3) / 1e9);
}

// Attention

void bench_attention(Backend& backend, int seq_len, int num_heads, int num_kv_heads, int head_dim) {
    auto q = HostBuf::alloc({1, num_heads, head_dim}, DType::kF32);
    auto keys = HostBuf::alloc({seq_len, num_kv_heads, head_dim}, DType::kF32);
    auto values = HostBuf::alloc({seq_len, num_kv_heads, head_dim}, DType::kF32);
    auto out = HostBuf::alloc({1, num_heads * head_dim}, DType::kF32);
    q.fill_random(1);
    keys.fill_random(2);
    values.fill_random(3);

    AttentionConfig cfg{
        .num_heads = num_heads,
        .num_kv_heads = num_kv_heads,
        .head_dim = head_dim,
        .scale = 1.0f / std::sqrt(static_cast<float>(head_dim)),
    };

    double ms;
    if (backend.HasDeviceKV()) {
        // Device KV path: configure, populate, then benchmark AttentionKV.
        backend.ConfigureDeviceKV(1, num_kv_heads, head_dim, seq_len);
        // Append all KV entries (outside the timed loop).
        for (int s = 0; s < seq_len; ++s) {
            // Slice one token's K/V from the full buffers.
            TensorView k_slice(
                static_cast<char*>(keys.view.data()) + s * num_kv_heads * head_dim * sizeof(float),
                DType::kF32,
                Shape({1, num_kv_heads, head_dim}));
            TensorView v_slice(
                static_cast<char*>(values.view.data()) + s * num_kv_heads * head_dim * sizeof(float),
                DType::kF32,
                Shape({1, num_kv_heads, head_dim}));
            backend.AppendKV(0, k_slice, v_slice, s);
        }
        sync_backend(backend);

        ms = bench_ms(
            [&] {
                auto s = backend.AttentionKV(out.view, q.view, 0, seq_len, cfg);
                if (!s.ok())
                    std::fprintf(stderr, "attention_kv failed: %s\n", s.message.c_str());
                sync_backend(backend);
            },
            20);
    } else {
        // Host KV path (CPU fallback).
        KVCacheView kv{
            .keys = keys.view.data(),
            .values = values.view.data(),
            .seq_len = seq_len,
            .num_kv_heads = num_kv_heads,
            .head_dim = head_dim,
            .dtype = DType::kF32,
        };
        ms = bench_ms(
            [&] {
                auto s = backend.Attention(out.view, q.view, kv, cfg);
                if (!s.ok())
                    std::fprintf(stderr, "attention failed: %s\n", s.message.c_str());
                sync_backend(backend);
            },
            20);
    }

    // FLOPs: QK (seq*seq per head) + PV (seq*seq per head), x2 for MACs.
    const double flops = 2.0 * static_cast<double>(seq_len) * static_cast<double>(num_heads) *
                         (2.0 * static_cast<double>(head_dim) * seq_len);
    std::printf("attention %6d : %9.3f ms  %7.1f GFLOP/s\n", seq_len, ms, flops / (ms / 1e3) / 1e9);
}

} // namespace

int main(int argc, char** argv) {
    bool use_metal = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            use_metal = std::strcmp(argv[++i], "metal") == 0;
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            std::printf("usage: %s [--backend cpu|metal]\n", argv[0]);
            return 0;
        }
    }

    std::unique_ptr<Backend> backend;
#if defined(__APPLE__)
    if (use_metal) {
        backend = std::make_unique<MetalBackend>();
    } else {
        backend = std::make_unique<CpuBackend>();
    }
#else
    if (use_metal) {
        std::fprintf(stderr, "error: --backend metal requires macOS\n");
        return 1;
    }
    backend = std::make_unique<CpuBackend>();
#endif

    std::printf("# mllm bench_ops  backend=%s commit=%s\n",
                use_metal ? "metal" : "cpu",
                bench::git_commit().c_str());
    std::printf("# rmsnorm hidden sizes (SPEC 13.2)\n");
    for (int h : {1024, 2048, 4096, 8192}) {
        bench_rmsnorm(*backend, h);
    }
    std::printf("# gemv f32 (out x in, LLaMA-like decode dims)\n");
    for (auto [o, i] : std::vector<std::pair<int, int>>{
             {4096, 4096}, {11008, 4096}, {4096, 11008}, {32000, 4096}}) {
        bench_gemv_f32(*backend, o, i);
    }
    std::printf("# gemv q8_0 fused dequant (metal decode path)\n");
    for (auto [o, i] : std::vector<std::pair<int, int>>{
             {4096, 4096}, {11008, 4096}, {4096, 11008}, {32000, 4096}}) {
        bench_gemv_q8_0(*backend, o, i);
    }
    std::printf("# attention seq lengths (32 heads, 8 kv heads, head_dim 128)\n");
    for (int seq : {128, 512, 2048, 4096}) {
        bench_attention(*backend, seq, 32, 8, 128);
    }

    return 0;
}
