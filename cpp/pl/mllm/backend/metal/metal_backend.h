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

#include <memory>
#include <span>
#include <string_view>

#include "cpp/pl/mllm/backend/backend.h"

namespace pl::mllm {

// Metal (Apple GPU) backend — optimized for decode-phase performance.
//
// Key optimizations:
//   * Shadow buffer cache: MTLBuffers are cached by host pointer so that
//     repeated ops on the same activation (e.g. hidden state through all
//     layers) avoid re-uploading. Invalidated by NotifyHostWrite.
//   * Deferred command buffer: a single MTLCommandBuffer is kept open across
//     multiple ops and only committed on SyncToHost / Synchronize, eliminating
//     per-op round-trip latency (~5-10 µs each on Apple Silicon).
//   * Device-resident KV cache: K/V storage lives in MTLBuffers on the GPU;
//     AppendKV and AttentionKV operate entirely on-device without uploading
//     the full cache each step.
//   * f16 weight preservation: f16 weights are uploaded as-is (half bandwidth);
//     the GEMV kernel reads half-precision natively via as_type<half>.
//   * Output buffers persist: the output MTLBuffer from one op is the input
//     MTLBuffer of the next, keeping the entire forward pass on-device.
//
// All Metal/Objective-C types live behind the opaque Impl (SPEC §2.2), so this
// header stays pure C++ and can be included from engine.cpp / cli.cpp.
class MetalBackend : public Backend {
public:
    MetalBackend();
    ~MetalBackend() override;

    MetalBackend(const MetalBackend&) = delete;
    MetalBackend& operator=(const MetalBackend&) = delete;

    Status ImportWeights(std::span<const TensorView> weights,
                         std::span<const std::string_view> names) override;

    Status MatMul(TensorView out, TensorView x, std::string_view weight_name) override;
    Status MatMulFused(std::span<TensorView> outs,
                       TensorView x,
                       std::span<const std::string_view> weight_names) override;
    Status RmsNorm(TensorView out, TensorView x, TensorView weight, float eps) override;
    Status RmsNormAdd(
        TensorView out, TensorView residual, TensorView add, TensorView weight, float eps) override;
    Status RoPE(TensorView q, TensorView k, int64_t position, const RopeConfig& config) override;
    Status Attention(TensorView out,
                     TensorView q,
                     const KVCacheView& kv,
                     const AttentionConfig& config) override;
    Status SwiGLU(TensorView out, TensorView gate, TensorView up) override;
    Status AddInPlace(TensorView x, TensorView residual) override;
    Status AddBiasInPlace(TensorView x, TensorView bias) override;
    Status Synchronize() override;

    // --- Device-residency hooks ---
    Status NotifyHostWrite(TensorView t) override;
    Status SyncToHost(TensorView t) override;

    // --- Device-resident KV cache ---
    bool HasDeviceKV() const override;
    Status ConfigureDeviceKV(int32_t num_layers,
                             int32_t num_kv_heads,
                             int32_t head_dim,
                             int32_t capacity) override;
    Status AppendKV(int32_t layer, TensorView key, TensorView value, int64_t position) override;
    Status AttentionKV(TensorView out,
                       TensorView q,
                       int32_t layer,
                       int64_t seq_len,
                       const AttentionConfig& config) override;
    Status AttentionPrefillKV(TensorView out,
                              TensorView q,
                              int32_t layer,
                              int64_t seq_base,
                              const AttentionConfig& config) override;

    // Test-only: inject a synthetic GPU error to exercise sticky error
    // propagation (real MTLCommandBuffer errors cannot be raised
    // deterministically from a unit test). Not for production use.
    void InjectGpuErrorForTest(Status error);

    // Opaque implementation handle (all Metal types live in the .mm TU).
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace pl::mllm
