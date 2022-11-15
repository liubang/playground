#pragma once

#include <cstdint>
#include <string>

#include "slice.h"

namespace highkyck {
namespace sstable {

void put_varint32(std::string* dst, uint32_t value);
void put_varint64(std::string* dst, uint64_t value);
void put_fixed32(std::string* dst, uint32_t value);
void put_fixed64(std::string* dst, uint64_t value);

bool get_varint32(Slice* input, uint32_t* value);
bool get_varint64(Slice* input, uint64_t* value);

char* encode_varint32(char* dst, uint32_t value);
char* encode_varint64(char* dst, uint64_t value);

inline void encode_fixed32(char* dst, uint32_t value) {
  uint8_t* const buffer = reinterpret_cast<uint8_t*>(dst);
  buffer[0] = static_cast<uint8_t>(value);
  buffer[1] = static_cast<uint8_t>(value >> 8);
  buffer[2] = static_cast<uint8_t>(value >> 16);
  buffer[3] = static_cast<uint8_t>(value >> 24);
}

inline void encode_fixed64(char* dst, uint64_t value) {
  uint8_t* const buffer = reinterpret_cast<uint8_t*>(dst);
  buffer[0] = static_cast<uint8_t>(value);
  buffer[1] = static_cast<uint8_t>(value >> 8);
  buffer[2] = static_cast<uint8_t>(value >> 16);
  buffer[3] = static_cast<uint8_t>(value >> 24);
  buffer[4] = static_cast<uint8_t>(value >> 32);
  buffer[5] = static_cast<uint8_t>(value >> 40);
  buffer[6] = static_cast<uint8_t>(value >> 48);
  buffer[7] = static_cast<uint8_t>(value >> 56);
}

inline uint32_t decode_fixed32(const char* ptr) {
  const uint8_t* const buffer = reinterpret_cast<const uint8_t*>(ptr);
  return (static_cast<uint32_t>(buffer[0])) |
         (static_cast<uint32_t>(buffer[1]) << 8) |
         (static_cast<uint32_t>(buffer[2]) << 16) |
         (static_cast<uint32_t>(buffer[3]) << 24);
}

inline uint64_t decode_fixed64(const char* ptr) {
  const uint8_t* const buffer = reinterpret_cast<const uint8_t*>(ptr);
  return (static_cast<uint64_t>(buffer[0])) |
         (static_cast<uint64_t>(buffer[1]) << 8) |
         (static_cast<uint64_t>(buffer[2]) << 16) |
         (static_cast<uint64_t>(buffer[3]) << 24) |
         (static_cast<uint64_t>(buffer[4]) << 32) |
         (static_cast<uint64_t>(buffer[5]) << 40) |
         (static_cast<uint64_t>(buffer[6]) << 48) |
         (static_cast<uint64_t>(buffer[7]) << 56);
}

const char* get_varint32_ptr_fallback(const char* p,
                                      const char* limit,
                                      uint32_t* value);

inline const char* get_varint32_ptr(const char* p,
                                    const char* limit,
                                    uint32_t* value) {
  if (p < limit) {
    uint32_t result = *(reinterpret_cast<const uint8_t*>(p));
    if ((result & 128) == 0) {
      *value = result;
      return p + 1;
    }
  }
  return get_varint32_ptr_fallback(p, limit, value);
}

const char* get_varint64_ptr(const char* p, const char* limit, uint64_t* value);

}  // namespace sstable
}  // namespace highkyck
