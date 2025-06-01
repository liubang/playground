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
#include <memory>
#include <vector>

namespace pl::old {

class Arena final {
public:
    Arena() = default;
    ~Arena() = default;
    // not copyable and moveabel
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) = delete;
    Arena& operator=(Arena&&) = delete;

    [[nodiscard]] char* allocate(std::size_t bytes);
    [[nodiscard]] char* allocate_aligned(std::size_t bytes);
    [[nodiscard]] std::size_t memory_usage() const;

    static constexpr int BLOCK_SIZE = 4096;
    static constexpr int POINTER_SIZE = 8;

private:
    [[nodiscard]] char* allocate_fallback(std::size_t bytes);
    [[nodiscard]] char* allocate_new_block(std::size_t block_bytes);

private:
    // allocation state
    char* alloc_ptr_{nullptr};
    std::size_t alloc_bytes_remaining_{0};
    // array of new[] allocated memory blocks
    std::vector<std::unique_ptr<char[]>> blocks_;
    std::atomic<std::size_t> memory_usage_{0};
};

} // namespace pl::old
