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

#include <cstdint>
#include <vector>

#include "cpp/pl/mllm/backend/backend.h"
#include "cpp/pl/mllm/core/buffer.h"
#include "cpp/pl/mllm/core/dtype.h"
#include "cpp/pl/mllm/core/status.h"
#include "cpp/pl/mllm/core/tensor.h"
#include "cpp/pl/mllm/model/config.h"

namespace pl::mllm {

// Contiguous, pre-allocated KV cache for all layers.
// Layout per layer: [max_tokens, num_kv_heads, head_dim] for K and V.
// MVP: no ring overwrite; overflow returns error (see SPEC §7.1).
class KVCache {
public:
    [[nodiscard]] static Result<KVCache> Create(const ModelConfig& config,
                                                int32_t max_tokens,
                                                DType dtype = DType::kF16);

    // Metadata-only cache for backends with device-resident K/V storage
    // (Backend::HasDeviceKV()). A shell tracks length/capacity so Engine
    // bounds checks and Model::Advance keep working, but its backing buffers
    // hold a single token only — Append/View must never be called on it.
    [[nodiscard]] static Result<KVCache> CreateShell(const ModelConfig& config, int32_t max_tokens);

    // Append one token's K/V for a layer.
    // key/value shape: [1, num_kv_heads, head_dim]
    Status Append(int32_t layer, TensorView key, TensorView value);

    // Append a contiguous run of tokens' K/V for a layer (batched prefill).
    // key/value shape: [n, num_kv_heads, head_dim]; appended at the current
    // length. The caller advances the length afterwards via Advance(n).
    Status AppendBatch(int32_t layer, TensorView key, TensorView value);

    // View of the valid cache range for a single layer.
    [[nodiscard]] KVCacheView View(int32_t layer) const noexcept;

    // Advance the valid length by n positions (default 1). Called by the
    // Model after all layers have appended for the corresponding tokens.
    void Advance(int32_t n = 1) noexcept;

    [[nodiscard]] int32_t length() const noexcept { return length_; }
    [[nodiscard]] int32_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] int32_t num_layers() const noexcept { return num_layers_; }
    [[nodiscard]] int32_t num_kv_heads() const noexcept { return num_kv_heads_; }
    [[nodiscard]] int32_t head_dim() const noexcept { return head_dim_; }
    [[nodiscard]] DType dtype() const noexcept { return dtype_; }

    void Clear() noexcept { length_ = 0; }

private:
    KVCache() = default;

    // Returns pointer to layer l's K data at the given token position.
    void* k_ptr(int32_t layer, int32_t pos) noexcept;
    void* v_ptr(int32_t layer, int32_t pos) noexcept;

    OwnedBuffer k_buffer_;
    OwnedBuffer v_buffer_;
    int32_t num_layers_ = 0;
    int32_t num_kv_heads_ = 0;
    int32_t head_dim_ = 0;
    int32_t capacity_ = 0;
    int32_t length_ = 0;
    DType dtype_ = DType::kF16;
    size_t elem_size_ = 0;        // bytes per element (2 for f16, 4 for f32)
    size_t per_token_stride_ = 0; // bytes per token per layer
};

} // namespace pl::mllm
