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
// Created: 2026/06/05 00:23

#pragma once

#include <cstdint>
#include <cstring>

#include "cpp/pl/bits/bits.h"

namespace pl::sstv2::codec {

// All disk formats use little-endian byte order.
// These functions encode/decode between host byte order and little-endian wire format.

// --- Unsigned encode/decode ---

inline void encode_fixed8(uint8_t* dst, uint8_t value) {
    dst[0] = value;
}

inline void encode_fixed16(uint8_t* dst, uint16_t value) {
    uint16_t le = Endian::little(value);
    std::memcpy(dst, &le, sizeof(le));
}

inline void encode_fixed32(uint8_t* dst, uint32_t value) {
    uint32_t le = Endian::little(value);
    std::memcpy(dst, &le, sizeof(le));
}

inline void encode_fixed64(uint8_t* dst, uint64_t value) {
    uint64_t le = Endian::little(value);
    std::memcpy(dst, &le, sizeof(le));
}

inline uint8_t decode_fixed8(const uint8_t* src) {
    return src[0];
}

inline uint16_t decode_fixed16(const uint8_t* src) {
    uint16_t le;
    std::memcpy(&le, src, sizeof(le));
    return Endian::little(le);
}

inline uint32_t decode_fixed32(const uint8_t* src) {
    uint32_t le;
    std::memcpy(&le, src, sizeof(le));
    return Endian::little(le);
}

inline uint64_t decode_fixed64(const uint8_t* src) {
    uint64_t le;
    std::memcpy(&le, src, sizeof(le));
    return Endian::little(le);
}

// --- Signed encode/decode (reinterpret to unsigned for wire format) ---

inline void encode_fixed8(uint8_t* dst, int8_t value) {
    uint8_t u;
    std::memcpy(&u, &value, 1);
    encode_fixed8(dst, u);
}

inline void encode_fixed16(uint8_t* dst, int16_t value) {
    uint16_t u;
    std::memcpy(&u, &value, sizeof(u));
    encode_fixed16(dst, u);
}

inline void encode_fixed32(uint8_t* dst, int32_t value) {
    uint32_t u;
    std::memcpy(&u, &value, sizeof(u));
    encode_fixed32(dst, u);
}

inline void encode_fixed64(uint8_t* dst, int64_t value) {
    uint64_t u;
    std::memcpy(&u, &value, sizeof(u));
    encode_fixed64(dst, u);
}

inline int8_t decode_fixed_i8(const uint8_t* src) {
    uint8_t u = decode_fixed8(src);
    int8_t v;
    std::memcpy(&v, &u, 1);
    return v;
}

inline int16_t decode_fixed_i16(const uint8_t* src) {
    uint16_t u = decode_fixed16(src);
    int16_t v;
    std::memcpy(&v, &u, sizeof(v));
    return v;
}

inline int32_t decode_fixed_i32(const uint8_t* src) {
    uint32_t u = decode_fixed32(src);
    int32_t v;
    std::memcpy(&v, &u, sizeof(v));
    return v;
}

inline int64_t decode_fixed_i64(const uint8_t* src) {
    uint64_t u = decode_fixed64(src);
    int64_t v;
    std::memcpy(&v, &u, sizeof(v));
    return v;
}

} // namespace pl::sstv2::codec
