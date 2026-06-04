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

namespace pl::sstv2::codec {

// All disk formats use little-endian byte order.

inline void encode_fixed8(uint8_t* dst, uint8_t value) { dst[0] = value; }

inline void encode_fixed16(uint8_t* dst, uint16_t value) {
    std::memcpy(dst, &value, sizeof(value));
}

inline void encode_fixed32(uint8_t* dst, uint32_t value) {
    std::memcpy(dst, &value, sizeof(value));
}

inline void encode_fixed64(uint8_t* dst, uint64_t value) {
    std::memcpy(dst, &value, sizeof(value));
}

inline uint8_t decode_fixed8(const uint8_t* src) { return src[0]; }

inline uint16_t decode_fixed16(const uint8_t* src) {
    uint16_t value;
    std::memcpy(&value, src, sizeof(value));
    return value;
}

inline uint32_t decode_fixed32(const uint8_t* src) {
    uint32_t value;
    std::memcpy(&value, src, sizeof(value));
    return value;
}

inline uint64_t decode_fixed64(const uint8_t* src) {
    uint64_t value;
    std::memcpy(&value, src, sizeof(value));
    return value;
}

} // namespace pl::sstv2::codec
