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

// Metal (Apple GPU) backend — MVP correctness-focused implementation.
// All Metal/Objective-C types live behind the opaque Impl (SPEC §2.2), so this
// header stays pure C++ and can be included from engine.cpp / cli.cpp.
//
// Compute model (MVP):
//   * ImportWeights converts every weight to f32 and uploads to a shared
//     MTLBuffer (Q8_0 is dequantized at import time).
//   * Each op synchronously uploads inputs, runs a shader (compiled at runtime
//     from shader_source.h) or MPSMatrixMultiplication for MatMul, then
//     downloads the f32 output back into the caller's TensorView.
//   * Shaders are f32-only; f16 inputs are converted on upload.
class MetalBackend : public Backend {
public:
    MetalBackend();
    ~MetalBackend() override;

    MetalBackend(const MetalBackend&) = delete;
    MetalBackend& operator=(const MetalBackend&) = delete;

    Status ImportWeights(std::span<const TensorView> weights,
                         std::span<const std::string_view> names) override;

    Status MatMul(TensorView out, TensorView x, std::string_view weight_name) override;
    Status RmsNorm(TensorView out, TensorView x, TensorView weight, float eps) override;
    Status RoPE(TensorView q, TensorView k, int64_t position, const RopeConfig& config) override;
    Status Attention(TensorView out,
                     TensorView q,
                     const KVCacheView& kv,
                     const AttentionConfig& config) override;
    Status SwiGLU(TensorView out, TensorView gate, TensorView up) override;
    Status AddInPlace(TensorView x, TensorView residual) override;
    Status Synchronize() override;

    // Opaque implementation handle (all Metal types live in the .mm TU).
    // Public so translation units can hold/dereference the unique_ptr.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace pl::mllm
