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
#include <cstdlib>
#include <new>
#include <utility>

#include "cpp/pl/mllm/core/status.h"

namespace pl::mllm {

// RAII owner of an aligned CPU allocation. Move-only.
class OwnedBuffer {
public:
    [[nodiscard]] static Result<OwnedBuffer> AllocateCpu(size_t bytes, size_t alignment) {
        if (bytes == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0) {
            return Status::Error(ErrorCode::kInvalidArgument, "AllocateCpu: bad size/alignment");
        }
        // posix_memalign requires alignment >= sizeof(void*).
        if (alignment < sizeof(void*)) {
            alignment = sizeof(void*);
        }
        void* ptr = nullptr;
        if (posix_memalign(&ptr, alignment, bytes) != 0) {
            return Status::Error(ErrorCode::kOutOfMemory, "AllocateCpu: posix_memalign failed");
        }
        OwnedBuffer buf(ptr, bytes, [](void* p) { std::free(p); }); // NOLINT
        return buf;
    }

    OwnedBuffer() = default;
    ~OwnedBuffer() { reset(); }

    OwnedBuffer(OwnedBuffer&& other) noexcept { *this = std::move(other); }
    OwnedBuffer& operator=(OwnedBuffer&& other) noexcept {
        if (this != &other) {
            reset();
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
            deleter_ = std::exchange(other.deleter_, nullptr);
        }
        return *this;
    }
    OwnedBuffer(const OwnedBuffer&) = delete;
    OwnedBuffer& operator=(const OwnedBuffer&) = delete;

    [[nodiscard]] void* data() noexcept { return data_; }
    [[nodiscard]] const void* data() const noexcept { return data_; }
    [[nodiscard]] size_t size() const noexcept { return size_; }

    void reset() noexcept {
        if (data_ != nullptr && deleter_ != nullptr) {
            deleter_(data_);
        }
        data_ = nullptr;
        size_ = 0;
    }

private:
    OwnedBuffer(void* data, size_t size, void (*deleter)(void*))
        : data_(data), size_(size), deleter_(deleter) {}

    void* data_ = nullptr;
    size_t size_ = 0;
    void (*deleter_)(void*) = nullptr;
};

} // namespace pl::mllm
