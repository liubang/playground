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

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "cpp/pl/mllm/backend/backend.h"
#include "cpp/pl/mllm/core/tensor.h"

namespace pl::mllm {

// CPU debug backend — reference implementation for correctness checking.
// Optimized for clarity, not speed (see SPEC §5.3).
class CpuBackend : public Backend {
public:
    CpuBackend() = default;

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
    Status AddBiasInPlace(TensorView x, TensorView bias) override;
    Status Synchronize() override { return {}; }

    // Test helper: direct access to imported weight by name.
    [[nodiscard]] const TensorView* FindWeight(std::string_view name) const;

private:
    // Named weight table. TensorViews are non-owning; the caller (Model)
    // guarantees the backing mmap / OwnedBuffer outlives the backend.
    std::unordered_map<std::string, TensorView> weights_;
};

} // namespace pl::mllm
