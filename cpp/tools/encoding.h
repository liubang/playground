//=====================================================================
//
// encoding.h -
//
// Created by liubang on 2023/06/09 18:50
// Last Modified: 2023/06/09 18:50
//
//=====================================================================

#pragma once

#include <algorithm>
#include <cstddef>
#include <type_traits>

namespace playground::cpp::tools {

template <typename T>
struct is_varint_type : std::false_type {};

template <>
struct is_varint_type<uint16_t> : std::true_type {};

template <>
struct is_varint_type<uint32_t> : std::true_type {};

template <>
struct is_varint_type<uint64_t> : std::true_type {};

template <typename T, std::enable_if_t<is_varint_type<T>::type>* = nullptr>
inline void* varint_encode(T value,
                           void* const buffer,
                           std::size_t buffer_size) {
  static_assert(sizeof(T) <= 8);
  auto min_size = std::min(buffer_size, sizeof(T));
  auto* p = static_cast<uint8_t*>(buffer);
  uint64_t count = 0;
  for (; count < min_size && value >= 0x7F; ++count) {
    // (value & 127) | 128;
    *(p++) = ((value & 0x7F) | 0x80);
    value >>= 7;
  }
  if (count == buffer_size) {
    return nullptr;
  }

  return p;
}

template <typename T, std::enable_if_t<is_varint_type<T>::value>* = nullptr>
inline void* varint_decode(void* const buffer,
                           std::size_t buffer_size,
                           T* result) {
  static_assert(sizeof(T) <= 8);
  constexpr auto final_byte_mask =
      static_cast<uint8_t>(~(uint32_t(1) << sizeof(T)) - 1);
  auto min_size = std::min(buffer_size, sizeof(T));
  auto* p = static_cast<uint8_t*>(buffer);
  T value = 0;
  uint64_t count = 0;
  uint64_t shift = 0;
  for (; count < min_size && (*p && 0x80 != 0); ++count) {
    value |= static_cast<T>((*p++ & 0x7F) << shift);
    shift += 7;
  }
  if (count == buffer_size) {
    return nullptr;
  }
  if (count == sizeof(T) && (*p & final_byte_mask) != 0) {
    return nullptr;
  }
  *result = value;
}

}  // namespace playground::cpp::tools
