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

#include <cstddef>
#include <cstdint>

#include "cpp/pl/mllm/core/buffer.h"
#include "cpp/pl/mllm/core/status.h"
#include "cpp/pl/mllm/core/tensor.h"

namespace pl::mllm {

// Bump allocator for per-token intermediate activations.
// Allocate until exhaustion, Reset() once per decode step; never frees pieces.
class ScratchArena {
public:
    static constexpr size_t kAlignment = 64;

    [[nodiscard]] static Result<ScratchArena> Create(size_t bytes) {
        auto buf = OwnedBuffer::AllocateCpu(bytes, kAlignment);
        if (!buf.ok()) {
            return buf.status();
        }
        ScratchArena arena(std::move(buf).value());
        return arena;
    }

    // Returns a contiguous tensor carved from the arena; never nullptr-checked.
    [[nodiscard]] Result<TensorView> AllocateTensor(Shape shape, DType dtype) noexcept {
        const int64_t numel = shape.numel();
        const size_t bytes = dtype_nbytes(dtype, numel);
        if (numel <= 0 || bytes == 0) {
            return Status::Error(ErrorCode::kInvalidArgument, "arena: bad tensor request");
        }
        if (offset_ + bytes > buffer_.size()) {
            return Status::Error(ErrorCode::kOutOfMemory, "arena: exhausted");
        }
        auto* ptr = static_cast<char*>(buffer_.data()) + offset_;
        offset_ += align_up(bytes);
        return TensorView(ptr, dtype, shape);
    }

    void Reset() noexcept { offset_ = 0; }
    [[nodiscard]] size_t capacity() const noexcept { return buffer_.size(); }
    [[nodiscard]] size_t used() const noexcept { return offset_; }

private:
    explicit ScratchArena(OwnedBuffer buffer) : buffer_(std::move(buffer)) {}

    static constexpr size_t align_up(size_t n) noexcept {
        return (n + kAlignment - 1) / kAlignment * kAlignment;
    }

    OwnedBuffer buffer_{};
    size_t offset_ = 0;
};

} // namespace pl::mllm
