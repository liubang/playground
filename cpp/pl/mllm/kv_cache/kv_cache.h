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

// Overflow behavior of the cache (SPEC §7).
//  * kStrict: append beyond capacity fails; the Engine rejects prompts that
//    do not fit (MVP semantics, §7.1).
//  * kRing: sliding window over the most recent `capacity` tokens. When an
//    append would overflow, the oldest tokens are dropped (a chunked
//    compaction, ceil(capacity/2) at a time, so the amortized cost per
//    token is O(1)) and the window origin advances. Physical slot i always holds
//    absolute position window_origin() + i, so views stay contiguous and
//    attention kernels need no wrap-around support (§7.2).
// RoPE positions stay absolute: attention scores depend only on relative
// offsets (p - j <= capacity), so no position remap or re-rotation is ever
// needed after a shift.
enum class KVCacheMode {
    kStrict,
    kRing,
};

// Contiguous, pre-allocated KV cache for all layers.
// Layout per layer: [max_tokens, num_kv_heads, head_dim] for K and V.
// Strict mode: no overwrite; overflow returns error (see SPEC §7.1).
// Ring mode: sliding-window compaction, never overflows (see SPEC §7.2).
class KVCache {
public:
    [[nodiscard]] static Result<KVCache> Create(const ModelConfig& config,
                                                int32_t max_tokens,
                                                DType dtype = DType::kF16,
                                                KVCacheMode mode = KVCacheMode::kStrict);

    // Metadata-only cache for backends with device-resident K/V storage
    // (Backend::HasDeviceKV()). A shell tracks length/capacity/origin so
    // Engine bounds checks and Model::Advance keep working, but its backing
    // buffers hold a single token only — Append/View must never be called
    // on it. In ring mode the Engine drives WindowShift() together with
    // Backend::ShiftKV() so the host shell and device buffers stay in sync.
    [[nodiscard]] static Result<KVCache> CreateShell(const ModelConfig& config,
                                                     int32_t max_tokens,
                                                     KVCacheMode mode = KVCacheMode::kStrict);

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
    [[nodiscard]] bool ring() const noexcept { return mode_ == KVCacheMode::kRing; }

    // Absolute position of physical slot 0, i.e. the number of tokens that
    // have been dropped by ring compaction. Always 0 in strict mode, so
    // `absolute_pos - window_origin()` is the physical slot in both modes.
    [[nodiscard]] int64_t window_origin() const noexcept { return origin_; }

    // Drop the oldest `drop` tokens: physical slots shift left by `drop`
    // (host caches memmove the data; shells only adjust bookkeeping — the
    // caller must shift any device-side storage itself) and the window
    // origin advances. Requires 0 <= drop <= length(). In strict mode this
    // is only valid for shells (a strict host cache never drops).
    Status WindowShift(int64_t drop);

    void Clear() noexcept {
        length_ = 0;
        origin_ = 0;
    }

private:
    KVCache() = default;

    // Returns pointer to layer l's K data at the given token position.
    void* k_ptr(int32_t layer, int32_t pos) noexcept;
    void* v_ptr(int32_t layer, int32_t pos) noexcept;

    // Number of tokens dropped per ring compaction (~amortized O(1) shifts).
    // Rounds up: dropping ceil(capacity/2) keeps at most floor(capacity/2),
    // so the retained range never overlaps the dropped one — a strict
    // requirement for device-side copies (Metal blits into the same buffer
    // have undefined behavior on overlap for odd capacities).
    [[nodiscard]] int32_t ring_shift() const noexcept { return (capacity_ + 1) / 2; }

    OwnedBuffer k_buffer_;
    OwnedBuffer v_buffer_;
    int32_t num_layers_ = 0;
    int32_t num_kv_heads_ = 0;
    int32_t head_dim_ = 0;
    int32_t capacity_ = 0;
    int32_t length_ = 0;
    int64_t origin_ = 0; // absolute position of physical slot 0 (ring mode)
    DType dtype_ = DType::kF16;
    KVCacheMode mode_ = KVCacheMode::kStrict;
    bool shell_ = false;          // metadata-only (device-resident K/V)
    size_t elem_size_ = 0;        // bytes per element (2 for f16, 4 for f32)
    size_t per_token_stride_ = 0; // bytes per token per layer
};

} // namespace pl::mllm
