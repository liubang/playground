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
// Created: 2026/07/17 22:26

#include "cpp/pl/sstv2/codec/ordered_uint.h"

namespace pl::sstv2::codec {

void encode_ordered_uint32(uint32_t value, std::string* dst) {
    if (value == 0) {
        dst->push_back('\0');
        return;
    }

    uint8_t width = 4;
    while (width > 1 && (value >> ((width - 1) * 8)) == 0) {
        --width;
    }
    dst->push_back(static_cast<char>(width));
    for (uint8_t i = width; i > 0; --i) {
        dst->push_back(static_cast<char>(value >> ((i - 1) * 8)));
    }
}

size_t decode_ordered_uint32(const uint8_t* src, size_t len, uint32_t* value) noexcept {
    if (src == nullptr || value == nullptr || len == 0) {
        return 0;
    }

    const uint8_t width = src[0];
    if (width == 0) {
        *value = 0;
        return 1;
    }
    if (width > 4 || len < static_cast<size_t>(width) + 1 || src[1] == 0) {
        return 0;
    }

    uint32_t decoded = 0;
    for (uint8_t i = 0; i < width; ++i) {
        decoded = (decoded << 8) | src[static_cast<size_t>(i) + 1];
    }
    *value = decoded;
    return static_cast<size_t>(width) + 1;
}

} // namespace pl::sstv2::codec
