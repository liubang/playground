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

#include "cpp/pl/mllm/kv_cache/kv_cache.h"

#include <cstring>

namespace pl::mllm {

Result<KVCache> KVCache::Create(const ModelConfig& config, int32_t max_tokens, DType dtype) {
    if (max_tokens <= 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "KVCache: max_tokens must be positive");
    }
    if (dtype != DType::kF16 && dtype != DType::kF32) {
        return Status::Error(ErrorCode::kUnsupported, "KVCache: only f16/f32 supported");
    }

    KVCache cache;
    cache.num_layers_ = config.num_layers;
    cache.num_kv_heads_ = config.num_kv_heads;
    cache.head_dim_ = config.effective_head_dim();
    cache.capacity_ = max_tokens;
    cache.dtype_ = dtype;
    cache.elem_size_ = dtype_type_size(dtype);
    cache.per_token_stride_ = static_cast<size_t>(cache.num_kv_heads_) *
                              static_cast<size_t>(cache.head_dim_) * cache.elem_size_;

    // Total buffer: [num_layers, max_tokens, num_kv_heads, head_dim]
    const size_t total_bytes = cache.per_token_stride_ * static_cast<size_t>(cache.num_layers_) *
                               static_cast<size_t>(max_tokens);
    auto buf = OwnedBuffer::AllocateCpu(total_bytes, 64);
    if (!buf.ok()) {
        return buf.status();
    }
    cache.k_buffer_ = std::move(buf).value();

    auto vbuf = OwnedBuffer::AllocateCpu(total_bytes, 64);
    if (!vbuf.ok()) {
        return vbuf.status();
    }
    cache.v_buffer_ = std::move(vbuf).value();

    std::memset(cache.k_buffer_.data(), 0, total_bytes);
    std::memset(cache.v_buffer_.data(), 0, total_bytes);

    return cache;
}

Result<KVCache> KVCache::CreateShell(const ModelConfig& config, int32_t max_tokens) {
    if (max_tokens <= 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "KVCache: max_tokens must be positive");
    }

    KVCache cache;
    cache.num_layers_ = config.num_layers;
    cache.num_kv_heads_ = config.num_kv_heads;
    cache.head_dim_ = config.effective_head_dim();
    cache.capacity_ = max_tokens;
    cache.dtype_ = DType::kF32;
    cache.elem_size_ = dtype_type_size(DType::kF32);
    cache.per_token_stride_ = static_cast<size_t>(cache.num_kv_heads_) *
                              static_cast<size_t>(cache.head_dim_) * cache.elem_size_;

    // Single-token backing buffers; the real K/V live on the device.
    auto kbuf = OwnedBuffer::AllocateCpu(cache.per_token_stride_, 64);
    if (!kbuf.ok()) {
        return kbuf.status();
    }
    cache.k_buffer_ = std::move(kbuf).value();

    auto vbuf = OwnedBuffer::AllocateCpu(cache.per_token_stride_, 64);
    if (!vbuf.ok()) {
        return vbuf.status();
    }
    cache.v_buffer_ = std::move(vbuf).value();
    return cache;
}

void* KVCache::k_ptr(int32_t layer, int32_t pos) noexcept {
    const size_t layer_offset =
        static_cast<size_t>(layer) * static_cast<size_t>(capacity_) * per_token_stride_;
    const size_t pos_offset = static_cast<size_t>(pos) * per_token_stride_;
    return static_cast<char*>(k_buffer_.data()) + layer_offset + pos_offset;
}

void* KVCache::v_ptr(int32_t layer, int32_t pos) noexcept {
    const size_t layer_offset =
        static_cast<size_t>(layer) * static_cast<size_t>(capacity_) * per_token_stride_;
    const size_t pos_offset = static_cast<size_t>(pos) * per_token_stride_;
    return static_cast<char*>(v_buffer_.data()) + layer_offset + pos_offset;
}

Status KVCache::Append(int32_t layer, TensorView key, TensorView value) {
    if (layer < 0 || layer >= num_layers_) {
        return Status::Error(ErrorCode::kInvalidArgument, "KVCache: layer out of range");
    }
    if (length_ >= capacity_) {
        return Status::Error(ErrorCode::kInvalidArgument, "KVCache: capacity exceeded");
    }
    if (key.dtype() != dtype_ || value.dtype() != dtype_) {
        return Status::Error(ErrorCode::kInvalidArgument, "KVCache: dtype mismatch");
    }
    if (key.shape().rank() != 3 || key.shape().dim(0) != 1 || key.shape().dim(1) != num_kv_heads_ ||
        key.shape().dim(2) != head_dim_) {
        return Status::Error(ErrorCode::kInvalidArgument, "KVCache: key shape mismatch");
    }
    if (value.shape() != key.shape()) {
        return Status::Error(ErrorCode::kInvalidArgument, "KVCache: value shape mismatch");
    }

    const size_t copy_bytes = per_token_stride_;
    std::memcpy(k_ptr(layer, length_), key.data(), copy_bytes);
    std::memcpy(v_ptr(layer, length_), value.data(), copy_bytes);

    return {};
}

void KVCache::Advance() noexcept {
    if (length_ < capacity_) {
        ++length_;
    }
}

KVCacheView KVCache::View(int32_t layer) const noexcept {
    // Returns the range of fully-advanced tokens. A caller that appends the
    // current token and reads the view before Advance() (the model's layer
    // forward) must add one to seq_len to include the pending position.
    return KVCacheView{
        .keys = const_cast<KVCache*>(this)->k_ptr(layer, 0),
        .values = const_cast<KVCache*>(this)->v_ptr(layer, 0),
        .seq_len = length_,
        .num_kv_heads = num_kv_heads_,
        .head_dim = head_dim_,
        .dtype = dtype_,
    };
}

} // namespace pl::mllm
