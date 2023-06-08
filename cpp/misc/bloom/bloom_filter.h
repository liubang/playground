//=====================================================================
//
// bloom_filter.h -
//
// Created by liubang on 2023/05/21 22:48
// Last Modified: 2023/05/21 22:48
//
//=====================================================================
#pragma once

#include "cpp/tools/binary.h"

#include <cassert>
#include <cstddef>
#include <memory>

namespace playground::cpp::misc::bloom {

class BloomFilter final {
 public:
  explicit BloomFilter(std::size_t bit_per_key);

  BloomFilter(const BloomFilter&) = delete;
  BloomFilter(const BloomFilter&&) = delete;
  BloomFilter& operator=(const BloomFilter&) = delete;
  BloomFilter& operator=(const BloomFilter&&) = delete;

  // lookup if bloomfilter contains data
  [[nodiscard]] bool contains(const tools::Binary& key,
                              const tools::Binary& filter) const;

  // add a member to bloomfilter
  void create(const tools::Binary* keys, std::size_t n, std::string* dst) const;

 private:
  std::size_t bits_per_key_;  // 每一个key需要占用多少bit
  std::size_t hash_count_;    // hash函数的个数
};

}  // namespace playground::cpp::misc::bloom
