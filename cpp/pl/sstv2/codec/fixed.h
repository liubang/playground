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
// Created: 2026/06/05 22:09

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "cpp/pl/sstv2/codec/endian.h"

namespace pl::sstv2::codec {

inline void append_fixed8(std::string* dst, uint8_t value) {
    dst->push_back(static_cast<char>(value));
}

inline void append_fixed16(std::string* dst, uint16_t value) {
    uint8_t buf[2];
    encode_fixed16(buf, value);
    dst->append(reinterpret_cast<const char*>(buf), sizeof(buf));
}

inline void append_fixed32(std::string* dst, uint32_t value) {
    uint8_t buf[4];
    encode_fixed32(buf, value);
    dst->append(reinterpret_cast<const char*>(buf), sizeof(buf));
}

inline void append_fixed64(std::string* dst, uint64_t value) {
    uint8_t buf[8];
    encode_fixed64(buf, value);
    dst->append(reinterpret_cast<const char*>(buf), sizeof(buf));
}

[[nodiscard]] inline uint32_t read_fixed32(std::string_view input, size_t offset) {
    return decode_fixed32(reinterpret_cast<const uint8_t*>(input.data() + offset));
}

[[nodiscard]] inline uint16_t read_fixed16(std::string_view input, size_t offset) {
    return decode_fixed16(reinterpret_cast<const uint8_t*>(input.data() + offset));
}

[[nodiscard]] inline uint64_t read_fixed64(std::string_view input, size_t offset) {
    return decode_fixed64(reinterpret_cast<const uint8_t*>(input.data() + offset));
}

} // namespace pl::sstv2::codec
