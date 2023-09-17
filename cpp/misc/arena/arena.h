//=====================================================================
//
// arena.h -
//
// Created by liubang on 2023/05/21 01:38
// Last Modified: 2023/05/21 01:38
//
//=====================================================================
#pragma once

#include <atomic>
#include <cassert>
#include <memory>
#include <vector>

namespace pl {

// Reference from Google's leveldb
class Arena {
public:
    Arena();
    ~Arena();
    Arena(const Arena &) = delete;
    Arena &operator=(const Arena &) = delete;

    inline char *allocate(std::size_t bytes) {
        assert(bytes > 0);
        if (bytes <= alloc_bytes_remaining_) {
            char *result = alloc_ptr_;
            alloc_ptr_ += bytes;
            alloc_bytes_remaining_ -= bytes;
            return result;
        }

        return allocate_fallback(bytes);
    }

    char *allocate_aligned(std::size_t bytes);

    [[nodiscard]] std::size_t memory_usage() const {
        return memory_usage_.load(std::memory_order_relaxed);
    }

private:
    char *allocate_fallback(std::size_t bytes);
    char *allocate_new_block(std::size_t block_bytes);

private:
    // allocation state
    char *alloc_ptr_;

    std::size_t alloc_bytes_remaining_;

    // array of new[] allocated memory blocks
    std::vector<char *> blocks_;

    std::atomic<std::size_t> memory_usage_;
};

} // namespace pl
