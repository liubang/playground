//=====================================================================
//
// encoding.h -
//
// Created by liubang on 2023/05/29 23:51
// Last Modified: 2023/05/29 23:51
//
//=====================================================================

#pragma once

#include "cpp/tools/bits.h"

#include <cstring>
#include <string>
#include <type_traits>

namespace playground::cpp::misc::sst {

/**
 * @brief [TODO:description]
 *
 * @tparam T [TODO:tparam]
 * @param dst [TODO:parameter]
 * @param value [TODO:parameter]
 */
template <typename T,
          typename std::enable_if<std::is_integral<T>::value &&
                                      !std::is_same<T, bool>::value,
                                  T>::type = 0>
void encodeInt(std::string* dst, T value) {
  // 这里统一用little endian
  value = playground::cpp::tools::Endian::little(value);
  dst->append(reinterpret_cast<const char*>(&value), sizeof(T));
}

/**
 * @brief [TODO:description]
 *
 * @tparam T [TODO:tparam]
 * @param input [TODO:parameter]
 * @return [TODO:return]
 */
template <typename T,
          typename std::enable_if<std::is_integral<T>::value &&
                                      !std::is_same<T, bool>::value,
                                  T>::type = 0>
T decodeInt(const char* input) {
  T value;
  std::size_t s = sizeof(T);
  memcpy(&value, input, s);
  // 这里统一用little endian
  value = playground::cpp::tools::Endian::little(value);
  return value;
}

}  // namespace playground::cpp::misc::sst
