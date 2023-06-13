//=====================================================================
//
// bloom_filter.cpp -
//
// Created by liubang on 2023/05/30 16:27
// Last Modified: 2023/05/30 16:27
//
//=====================================================================

#include "cpp/misc/bloom/bloom_filter.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#include "cpp/misc/hash/murmurhash2.h"

namespace pl::misc::bloom {

BloomFilter::BloomFilter(std::size_t bit_per_key) : bits_per_key_(bit_per_key) {
  hash_count_ = static_cast<size_t>(bit_per_key * 0.69);
  if (hash_count_ < 1) hash_count_ = 1;
  if (hash_count_ > 30) hash_count_ = 30;
}

bool BloomFilter::contains(const tools::Binary& key,
                           const tools::Binary& filter) const {
  std::size_t len = filter.size();
  const size_t bits = (len - 1) << 3;
  auto* array = reinterpret_cast<uint8_t*>(const_cast<char*>(filter.data()));
  const size_t hash_count = array[len - 1];

  uint64_t seed = 0;
  pl::misc::hash::CMurmurHash64 hasher;
  for (std::size_t i = 0; i < hash_count; ++i) {
    hasher.begin(seed);
    hasher.add(key.data(), key.size(), false);
    seed = hasher.end();
    uint64_t bit_index = seed % bits;
    if ((array[bit_index >> 3] &
         static_cast<uint8_t>(1 << (bit_index & 0x7))) == 0) {
      return false;
    }
  }
  return true;
}

void BloomFilter::create(const tools::Binary* keys, std::size_t n,
                         std::string* dst) const {
  std::size_t bit_count = (n * bits_per_key_) << 3;
  uint64_t actual_bit_count = 8;
  while (actual_bit_count < bit_count) {
    actual_bit_count <<= 1;
  }

  uint64_t actual_byte_count = actual_bit_count >> 3;

  const size_t init_size = dst->size();
  dst->resize(init_size + actual_byte_count, 0);
  dst->push_back(static_cast<uint8_t>(
      hash_count_));  // 将 hash_count写入filter最后一个字节

  auto* array = reinterpret_cast<uint8_t*>(&((*dst)[init_size]));

  pl::misc::hash::CMurmurHash64 hasher;
  for (std::size_t i = 0; i < n; ++i) {
    uint64_t seed = 0;
    const auto key = keys[i];
    for (std::size_t j = 0; j < hash_count_; ++j) {
      hasher.begin(seed);
      hasher.add(key.data(), key.size(), false);
      seed = hasher.end();
      uint64_t bit_index = seed % actual_bit_count;
      array[bit_index >> 3] |= static_cast<uint8_t>(1 << (bit_index & 0x7));
    }
  }
}

}  // namespace pl::misc::bloom
