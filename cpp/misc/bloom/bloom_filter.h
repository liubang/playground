//=====================================================================
//
// bloom_filter.h -
//
// Created by liubang on 2023/05/21 22:48
// Last Modified: 2023/05/21 22:48
//
//=====================================================================
#pragma once

#include <cassert>
#include <cstddef>
#include <memory>

#include "cpp/tools/binary.h"

namespace pl {

class BloomFilter final {
public:
    explicit BloomFilter(std::size_t bit_per_key);

    BloomFilter(const BloomFilter &) = delete;
    BloomFilter(const BloomFilter &&) = delete;
    BloomFilter &operator=(const BloomFilter &) = delete;
    BloomFilter &operator=(const BloomFilter &&) = delete;

    // lookup if bloomfilter contains data
    [[nodiscard]] bool contains(const Binary &key, const Binary &filter) const;

    // add a member to bloomfilter
    void create(const Binary *keys, std::size_t n, std::string *dst) const;

private:
    std::size_t bits_per_key_; // 每一个key需要占用多少bit
    std::size_t hash_count_;   // hash函数的个数
};

} // namespace pl
