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

#include <cstddef>
#include <cstdint>

#include "cpp/pl/sstv2/types/data_type.h"

namespace pl::sstv2::types {

// Encodes sub-column metadata into a single byte:
//   [DataType:6][C:1][B:1]
// - DataType (bits 0-5): the sub-column's data type
// - C (bit 6): set if this is a nested sub-column of a compound type
// - B (bit 7): set if this sub-column carries a null bitmap
//
// Uses explicit bit manipulation instead of C++ bitfields for
// guaranteed cross-platform layout.
struct Flag {
    DataType type;
    bool compound_bit;
    bool bitmap_bit;

    static Flag from_byte(std::byte b) {
        auto raw = static_cast<uint8_t>(b);
        return Flag{
            .type = static_cast<DataType>(raw & 0x3F),
            .compound_bit = static_cast<bool>((raw >> 6) & 1),
            .bitmap_bit = static_cast<bool>((raw >> 7) & 1),
        };
    }

    [[nodiscard]] std::byte to_byte() const {
        uint8_t raw = static_cast<uint8_t>(type) & 0x3F;
        raw |= static_cast<uint8_t>(compound_bit) << 6;
        raw |= static_cast<uint8_t>(bitmap_bit) << 7;
        return static_cast<std::byte>(raw);
    }
};

} // namespace pl::sstv2::types
