#include <cassert>
#include <memory>
#include "highkyck/hash/murmurhash2.h"

#include "bloom_filter.h"

namespace highkyck {
namespace bloom {
BloomFilter::BloomFilter(const uint64_t bit_count) {
  uint64_t actual_bit_count = 8;
  while (actual_bit_count < bit_count) {
    actual_bit_count <<= 1;
  }

  assert((actual_bit_count & 0x7) == 0);
  assert(
      (actual_bit_count > 0) &&
      (actual_bit_count & (actual_bit_count - 1)) == 0);

  header_.magic_code = MAGIC_CODE;
  header_.version = DEFAULT_VERSION;
  header_.hash_count = DEFAULT_HASHCOUNT;
  header_.bit_count = actual_bit_count;
  header_.member_count = 0;
  header_.checksum = 0;

  uint64_t actual_byte_count = actual_bit_count >> 3;
  bits_ = std::make_unique<uint8_t[]>(actual_byte_count);
  std::memset(bits_.get(), 0, actual_byte_count);
}

bool BloomFilter::contains(const void* const data, uint64_t length) const {
  assert(data != nullptr && length != 0);
  uint64_t seed = 0;
  highkyck::hash::CMurmurHash64 hasher;
  for (uint32_t i = 0; i < header_.hash_count; ++i) {
    hasher.begin(seed);
    hasher.add(data, length, false);
    seed = hasher.end();
    if (!get_bit(seed % header_.bit_count)) {
      return false;
    }
  }
  return true;
}

void BloomFilter::insert(const void* const data, uint64_t length) {
  assert(data != nullptr && length != 0);
  uint64_t seed = 0;
  highkyck::hash::CMurmurHash64 hasher;
  for (uint32_t i = 0; i < header_.hash_count; ++i) {
    hasher.begin(seed);
    hasher.add(data, length, false);
    seed = hasher.end();
    set_bit(seed % header_.bit_count);
  }
  header_.member_count++;
}
} // namespace bloom
} // namespace highkyck
