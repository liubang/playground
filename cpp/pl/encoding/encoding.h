// Copyright (c) 2024 The Authors. All rights reserved.
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

#pragma once

#include <cassert>
#include <cstdint>
#include <string>
#include <type_traits>

namespace pl {

template <typename T> struct is_varint_type : std::false_type {};

template <> struct is_varint_type<uint16_t> : std::true_type {};
template <> struct is_varint_type<uint32_t> : std::true_type {};
template <> struct is_varint_type<uint64_t> : std::true_type {};

template <typename T, std::enable_if_t<is_varint_type<T>::value>* = nullptr>
bool varint_encode(T value, std::string* dist) {
    static_assert(sizeof(T) <= 8);
    assert(dist != nullptr);

    constexpr auto size = sizeof(T);
    char buffer[size];
    auto* p = reinterpret_cast<uint8_t*>(buffer);
    size_t count = 0;
    for (; count < size && value >= 0x7F; ++count) {
        // (value & 127) | 128;
        *(p++) = ((value & 0x7F) | 0x80);
        value >>= 7;
    }

    if (count == size) {
        return false;
    }

    dist->append(buffer, count);

    return true;
}

template <typename T, std::enable_if_t<is_varint_type<T>::value>* = nullptr>
bool varint_decode(std::string_view buffer, T* result) {
    static_assert(sizeof(T) <= 8);
    constexpr auto final_byte_mask = static_cast<uint8_t>(~(uint32_t(1) << sizeof(T)) - 1);
    constexpr auto size = sizeof(T);
    const auto* p = reinterpret_cast<const uint8_t*>(buffer.data());
    T value = 0;
    uint64_t count = 0;
    uint64_t shift = 0;
    for (; count < size && (*p && 0x80 != 0); ++count) {
        value |= static_cast<T>((*p++ & 0x7F) << shift);
        shift += 7;
    }
    if (count == size) {
        return false;
    }
    if (count == sizeof(T) && (*p & final_byte_mask) != 0) {
        return false;
    }

    *result = value;
    return true;
}

} // namespace pl
