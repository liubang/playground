// Copyright (c) 2025 The Authors. All rights reserved.
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

#include "cpp/pl/utility/utility.h"

#include <memory>
#include <vector>

namespace pl {

class Arena : public DisableCopyAndMove {
public:
    explicit Arena(std::size_t initial_block_size = 64 * 1024)
        : default_block_size_(initial_block_size) {
        allocate_new_block(initial_block_size);
    }

    ~Arena() = default;

    void* allocate(std::size_t size, std::size_t alignment = alignof(std::max_align_t)) {
        if (size == 0) {
            return nullptr;
        }
        size = align_up(size, alignment);

        if (current_block_ids_ < blocks_.size()) {
            auto& current_block = blocks_[current_block_ids_];
            std::size_t aligned_used = align_up(current_block.used, alignment);
            if (aligned_used + size <= current_block.size) {
                void* ptr = current_block.data.get() + aligned_used;
                current_block.used = aligned_used + size;
                return ptr;
            }
        }

        allocate_new_block(size);
        auto& new_block = blocks_[current_block_ids_];
        void* ptr = new_block.data.get();
        new_block.used = size;
        return ptr;
    }

    template <typename T, typename... Args> T* allocate_object(Args&&... args) {
        static_assert(std::is_destructible_v<T>, "Type must be destructible");

        void* ptr = allocate(sizeof(T), alignof(T));
        if (!ptr) {
            throw std::bad_alloc{};
        }

        try {
            return new (ptr) T(std::forward<Args>(args)...);
        } catch (...) {
            throw;
        }
    }

    void reset() noexcept {
        if (!blocks_.empty()) {
            blocks_[0].used = 0;
            blocks_.erase(blocks_.begin() + 1, blocks_.end());
            current_block_ids_ = 0;
        }
    }

    struct Stats {
        std::size_t total_allocated;
        std::size_t total_used;
        std::size_t block_count;
        double fragmentation_ratio;
    };

    [[nodiscard]] Stats get_stats() const noexcept {
        Stats stats{};
        stats.block_count = blocks_.size();

        for (const auto& block : blocks_) {
            stats.total_allocated += block.size;
            stats.total_used += block.used;
        }

        stats.fragmentation_ratio =
            stats.total_allocated > 0
                ? 1.0 - (static_cast<double>(stats.total_used) / stats.total_allocated)
                : 0.0;

        return stats;
    }

    [[nodiscard]] std::size_t available_in_current_block() const noexcept {
        if (current_block_ids_ >= blocks_.size()) {
            return 0;
        }

        const auto& current_block = blocks_[current_block_ids_];
        return current_block.size - current_block.used;
    }

private:
    static constexpr std::size_t align_up(std::size_t size, std::size_t alignment) noexcept {
        return (size + alignment - 1) & ~(alignment - 1);
    }

    void allocate_new_block(std::size_t min_size) {
        std::size_t block_size = std::max(default_block_size_, min_size);
        blocks_.emplace_back(block_size);
        current_block_ids_ = blocks_.size() - 1;
    }

private:
    struct Block {
        std::unique_ptr<std::byte[]> data;
        std::size_t size = 0;
        std::size_t used = 0;

        Block(std::size_t block_size)
            : data(std::make_unique<std::byte[]>(block_size)), size(block_size) {}
    };

    std::vector<Block> blocks_;
    std::size_t default_block_size_{0};
    std::size_t current_block_ids_{0};
};

} // namespace pl
