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

#include "cpp/pl/sstv2/encode/varints.h"

namespace pl::sstv2::encode {

size_t Varints::encode_uint32(uint32_t value, std::byte* dst) {
    size_t i = 0;
    while (value >= 0x80) {
        dst[i++] = static_cast<std::byte>((value & 0x7F) | 0x80);
        value >>= 7;
    }
    dst[i++] = static_cast<std::byte>(value);
    return i;
}

size_t Varints::encode_uint64(uint64_t value, std::byte* dst) {
    size_t i = 0;
    while (value >= 0x80) {
        dst[i++] = static_cast<std::byte>((value & 0x7F) | 0x80);
        value >>= 7;
    }
    dst[i++] = static_cast<std::byte>(value);
    return i;
}

std::pair<uint32_t, size_t> Varints::decode_uint32(std::span<const std::byte> src) {
    uint32_t result = 0;
    size_t i = 0;
    unsigned shift = 0;
    while (i < src.size() && i < kMaxVarint32Bytes) {
        auto b = static_cast<uint8_t>(src[i]);
        result |= static_cast<uint32_t>(b & 0x7F) << shift;
        ++i;
        if ((b & 0x80) == 0) {
            return {result, i};
        }
        shift += 7;
    }
    // Malformed varint — return what we have
    return {result, i};
}

std::pair<uint64_t, size_t> Varints::decode_uint64(std::span<const std::byte> src) {
    uint64_t result = 0;
    size_t i = 0;
    unsigned shift = 0;
    while (i < src.size() && i < kMaxVarint64Bytes) {
        auto b = static_cast<uint8_t>(src[i]);
        result |= static_cast<uint64_t>(b & 0x7F) << shift;
        ++i;
        if ((b & 0x80) == 0) {
            return {result, i};
        }
        shift += 7;
    }
    // Malformed varint — return what we have
    return {result, i};
}

} // namespace pl::sstv2::encode
