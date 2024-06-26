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

#include <bit>
#include <cstdint>
#include <type_traits>

namespace pl {

namespace detail {

constexpr auto kIsLittleEndian = __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__;
constexpr auto kIsBigEndian = !kIsLittleEndian;

static inline uint8_t byteswap_gen(uint8_t v) { return uint8_t(v); }

static inline uint16_t byteswap_gen(uint16_t v) { return __builtin_bswap16(v); }

static inline uint32_t byteswap_gen(uint32_t v) { return __builtin_bswap32(v); }

static inline uint64_t byteswap_gen(uint64_t v) { return __builtin_bswap64(v); }

template <std::size_t Size> struct uint_types_by_size;

template <> struct uint_types_by_size<1> {
    using type = uint8_t;
};

template <> struct uint_types_by_size<2> {
    using type = uint16_t;
};

template <> struct uint_types_by_size<4> {
    using type = uint32_t;
};

template <> struct uint_types_by_size<8> {
    using type = uint64_t;
};

template <class T> struct EndianInt {
    static_assert((std::is_integral_v<T> && !std::is_same_v<T, bool>) ||
                      std::is_floating_point_v<T>,
                  "template type parameter must be non-bool integral or floating point");

    static T swap(T x) {
        constexpr auto s = sizeof(T);
        using B = typename uint_types_by_size<s>::type;
        return std::bit_cast<T>(byteswap_gen(std::bit_cast<B>(x)));
    }

    static T big(T x) { return kIsLittleEndian ? EndianInt::swap(x) : x; }
    static T little(T x) { return kIsBigEndian ? EndianInt::swap(x) : x; }
};

} // namespace detail

class Endian {
public:
    enum class Order : uint8_t {
        LITTLE,
        BIG,
    };

    static constexpr Order order = detail::kIsLittleEndian ? Order::LITTLE : Order::BIG;

    template <class T> static T swap(T x) { return detail::EndianInt<T>::swap(x); }

    template <class T> static T big(T x) { return detail::EndianInt<T>::big(x); }

    template <class T> static T little(T x) { return detail::EndianInt<T>::little(x); }
};

} // namespace pl
