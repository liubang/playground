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

namespace playground::cpp::misc::bloom {

class BloomFilter final {
 public:
  static constexpr uint32_t MAGIC_CODE = 0x424c4f4d;
  static constexpr uint32_t DEFAULT_VERSION = 0;
  static constexpr uint32_t DEFAULT_HASHCOUNT = 4;
  static constexpr uint64_t DEFAULT_BIT_COUNT = static_cast<const uint64_t>(16 * 1024 * 1024 * 8);

 public:
  explicit BloomFilter(const uint64_t bit_cout);
  ~BloomFilter() = default;
  BloomFilter(const BloomFilter&) = delete;
  BloomFilter(const BloomFilter&&) = delete;
  BloomFilter& operator=(const BloomFilter&) = delete;
  BloomFilter& operator=(const BloomFilter&&) = delete;

  [[nodiscard]] uint32_t get_magic_code() const { return header_.magic_code; }

  [[nodiscard]] uint32_t get_version() const { return header_.version; }

  [[nodiscard]] uint32_t get_hash_count() const { return header_.hash_count; }

  [[nodiscard]] uint64_t get_bit_count() const { return header_.bit_count; }

  [[nodiscard]] uint64_t get_member_count() const { return header_.member_count; }

  [[nodiscard]] uint64_t get_checksum() const { return header_.checksum; }

  [[nodiscard]] uint64_t get_memory_size() const {
    // 1 byte equals to 8 bits
    return sizeof(BloomFilter) + (get_bit_count() >> 3);
  }

  // lookup if bloomfilter contains data
  bool contains(const void* const data, uint64_t length) const;

  // add a member to bloomfilter
  void insert(const void* const data, uint64_t length);

 private:
  [[nodiscard]] bool get_bit(const uint64_t bit_index) const {
    assert(bits_ != nullptr && bit_index < header_.bit_count);
    return (bits_.get()[bit_index >> 3] & static_cast<uint8_t>(1 << (bit_index & 0x7))) != 0;
  }

  void set_bit(const uint64_t bit_index) {
    assert(bits_ != nullptr && bit_index < header_.bit_count);
    bits_.get()[bit_index >> 3] |= static_cast<uint8_t>(1 << (bit_index & 0x7));
  }

 private:
#pragma pack(push, 1)
  struct Header {
    uint32_t magic_code;
    uint32_t version;
    uint32_t hash_count;
    uint64_t bit_count;
    uint64_t member_count;
    uint64_t checksum;

    bool is_valid() {
      if (magic_code != MAGIC_CODE) {
        return false;
      }
      if (version != DEFAULT_VERSION) {
        return false;
      }
      if (hash_count != DEFAULT_HASHCOUNT) {
        return false;
      }
      if ((bit_count & 0x7) != 0) {
        return false;
      }
      if (bit_count == 0 || (bit_count & (bit_count - 1)) != 0) {
        return false;
      }
      return true;
    }
  };
#pragma pack(pop)

  Header header_;
  std::unique_ptr<uint8_t[]> bits_;
};

}  // namespace playground::cpp::misc::bloom
