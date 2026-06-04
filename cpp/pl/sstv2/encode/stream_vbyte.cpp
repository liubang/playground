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

#include "cpp/pl/sstv2/encode/stream_vbyte.h"

#include <cstring>

namespace pl::sstv2::encode {

namespace {

// Returns the number of bytes needed to store value (1-4)
inline uint8_t byte_width(uint32_t value) {
    if (value < (1u << 8))
        return 1;
    if (value < (1u << 16))
        return 2;
    if (value < (1u << 24))
        return 3;
    return 4;
}

inline void write_value(uint32_t value, uint8_t width, std::byte* dst) {
    // Little-endian write of the lowest `width` bytes
    std::memcpy(dst, &value, width);
}

inline uint32_t read_value(const std::byte* src, uint8_t width) {
    uint32_t value = 0;
    std::memcpy(&value, src, width);
    return value;
}

} // namespace

size_t StreamVByte::encode(std::span<const uint32_t> values, std::byte* dst) {
    const size_t count = values.size();
    if (count == 0)
        return 0;

    const size_t num_control_bytes = (count + 3) / 4;
    std::byte* control_ptr = dst;
    std::byte* data_ptr = dst + num_control_bytes;

    // Zero out control bytes so partial groups are clean
    std::memset(control_ptr, 0, num_control_bytes);

    for (size_t i = 0; i < count; i += 4) {
        uint8_t control = 0;
        const size_t group_size = std::min<size_t>(4, count - i);

        for (size_t j = 0; j < group_size; ++j) {
            uint8_t width = byte_width(values[i + j]);
            // Store width-1 in 2-bit slot (0=1byte, 1=2bytes, 2=3bytes, 3=4bytes)
            control |= static_cast<uint8_t>((width - 1) << (j * 2));
            write_value(values[i + j], width, data_ptr);
            data_ptr += width;
        }

        *control_ptr++ = static_cast<std::byte>(control);
    }

    return static_cast<size_t>(data_ptr - dst);
}

void StreamVByte::decode(std::span<const std::byte> src, size_t count, uint32_t* dst) {
    if (count == 0)
        return;

    const size_t num_control_bytes = (count + 3) / 4;
    const std::byte* control_ptr = src.data();
    const std::byte* data_ptr = src.data() + num_control_bytes;

    for (size_t i = 0; i < count; i += 4) {
        uint8_t control = static_cast<uint8_t>(*control_ptr++);
        const size_t group_size = std::min<size_t>(4, count - i);

        for (size_t j = 0; j < group_size; ++j) {
            uint8_t width = ((control >> (j * 2)) & 0x03) + 1;
            dst[i + j] = read_value(data_ptr, width);
            data_ptr += width;
        }
    }
}

} // namespace pl::sstv2::encode
