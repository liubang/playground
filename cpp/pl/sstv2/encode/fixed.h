// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)
// Created: 2026/06/04 12:01

#pragma once

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace pl::sstv2::encode {

namespace detail {

// Byte-swap helper using compiler builtins for optimal codegen
template <typename T>
    requires std::is_arithmetic_v<T>
constexpr T byte_swap(T value) noexcept {
    if constexpr (sizeof(T) == 1) {
        return value;
    } else if constexpr (sizeof(T) == 2) {
        auto v = static_cast<uint16_t>(value);
        v = __builtin_bswap16(v);
        return static_cast<T>(v);
    } else if constexpr (sizeof(T) == 4) {
        auto v = static_cast<uint32_t>(value);
        v = __builtin_bswap32(v);
        return static_cast<T>(v);
    } else if constexpr (sizeof(T) == 8) {
        auto v = static_cast<uint64_t>(value);
        v = __builtin_bswap64(v);
        return static_cast<T>(v);
    }
}

} // namespace detail

template <typename T>
    requires std::is_arithmetic_v<T>
void encode_fixed(T value, std::byte* dst) {
    if constexpr (std::endian::native != std::endian::little) {
        value = detail::byte_swap(value);
    }
    std::memcpy(dst, &value, sizeof(T));
}

template <typename T>
    requires std::is_arithmetic_v<T>
T decode_fixed(const std::byte* src) {
    T value;
    std::memcpy(&value, src, sizeof(T));
    if constexpr (std::endian::native != std::endian::little) {
        value = detail::byte_swap(value);
    }
    return value;
}

} // namespace pl::sstv2::encode
