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

#include "cpp/pl/sstv2/codec/varint.h"

namespace pl::sstv2::codec {

size_t encode_varint(uint64_t value, uint8_t* dst) noexcept {
    size_t n = 0;
    while (value >= 0x80) {
        dst[n++] = static_cast<uint8_t>(value | 0x80);
        value >>= 7;
    }
    dst[n++] = static_cast<uint8_t>(value);
    return n;
}

size_t decode_varint(const uint8_t* src, size_t len, uint64_t* value) noexcept {
    if (value == nullptr || (src == nullptr && len != 0)) {
        return 0;
    }
    uint64_t result = 0;
    for (size_t i = 0; i < len && i < 10; ++i) {
        uint64_t byte = src[i];
        // The 10th byte (i==9) contributes bits 63, only bit 0 is valid.
        if (i == 9 && (byte & 0x7E) != 0) {
            return 0; // overflow: would exceed uint64
        }
        result |= (byte & 0x7F) << (7 * i);
        if ((byte & 0x80) == 0) {
            *value = result;
            return i + 1;
        }
    }
    return 0; // truncated or overflow
}

size_t varint_length(uint64_t value) noexcept {
    size_t n = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++n;
    }
    return n;
}

} // namespace pl::sstv2::codec
