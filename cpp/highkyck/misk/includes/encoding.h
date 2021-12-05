#pragma once

#include <cstdint>

namespace test {
inline uint32_t DecodeFixed32(const char* ptr)
{
  const uint8_t* const buffer = reinterpret_cast<const uint8_t*>(ptr);

  // Recent clang and gcc optimize this to a single mov / ldr instruction.
  return (static_cast<uint32_t>(buffer[0])) | (static_cast<uint32_t>(buffer[1]) << 8)
         | (static_cast<uint32_t>(buffer[2]) << 16) | (static_cast<uint32_t>(buffer[3]) << 24);
}

inline uint64_t DecodeFixed64(const char* ptr)
{
  const uint8_t* const buffer = reinterpret_cast<const uint8_t*>(ptr);

  // Recent clang and gcc optimize this to a single mov / ldr instruction.
  return (static_cast<uint64_t>(buffer[0])) | (static_cast<uint64_t>(buffer[1]) << 8)
         | (static_cast<uint64_t>(buffer[2]) << 16) | (static_cast<uint64_t>(buffer[3]) << 24)
         | (static_cast<uint64_t>(buffer[4]) << 32) | (static_cast<uint64_t>(buffer[5]) << 40)
         | (static_cast<uint64_t>(buffer[6]) << 48) | (static_cast<uint64_t>(buffer[7]) << 56);
}
}  // namespace test
