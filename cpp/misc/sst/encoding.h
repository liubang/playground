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

namespace pl {

/**
 * @brief
 *
 * @tparam T
 * @param dst
 * @param value
 */
template <typename T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, T> = 0>
void encodeInt(std::string* dst, T value) {
    // 这里统一用little endian
    value = pl::Endian::little(value);
    dst->append(reinterpret_cast<const char*>(&value), sizeof(T));
}

/**
 * @brief
 *
 * @tparam T
 * @param input
 * @return
 */
template <typename T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, T> = 0>
T decodeInt(const char* input) {
    T value;
    std::size_t s = sizeof(T);
    memcpy(&value, input, s);
    // 这里统一用little endian
    value = pl::Endian::little(value);
    return value;
}

} // namespace pl
