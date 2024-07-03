// Copyright (c) 2024 The Authors. All rights reserved.
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

#pragma once

#include <atomic>
#include <cassert>
#include <vector>

namespace pl {

class Arena {
public:
    Arena();
    ~Arena();

    // not copyable and moveabel
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) = delete;
    Arena& operator=(Arena&&) = delete;

    inline char* allocate(std::size_t bytes) {
        assert(bytes > 0);
        if (bytes <= alloc_bytes_remaining_) {
            char* result = alloc_ptr_;
            alloc_ptr_ += bytes;
            alloc_bytes_remaining_ -= bytes;
            return result;
        }

        return allocate_fallback(bytes);
    }

    char* allocate_aligned(std::size_t bytes);

    [[nodiscard]] std::size_t memory_usage() const {
        // 只需要保持原子性，不需要确保执行序
        return memory_usage_.load(std::memory_order_relaxed);
    }

    static constexpr int BLOCK_SIZE = 4096;
    static constexpr int POINTER_SIZE = 8;

private:
    char* allocate_fallback(std::size_t bytes);
    char* allocate_new_block(std::size_t block_bytes);

private:
    // allocation state
    char* alloc_ptr_;

    std::size_t alloc_bytes_remaining_;

    // array of new[] allocated memory blocks
    std::vector<char*> blocks_;

    std::atomic<std::size_t> memory_usage_;
};

} // namespace pl
