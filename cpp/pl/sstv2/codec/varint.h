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

#include <cstddef>
#include <cstdint>
#include <string>

namespace pl::sstv2::codec {

// LEB128 variable-length integer encoding.
// 7 bits per byte, MSB=1 means more bytes follow.

// Encodes value into dst. Returns number of bytes written (1-10).
// Caller must ensure dst has at least 10 bytes available.
[[nodiscard]] size_t encode_varint(uint64_t value, uint8_t* dst) noexcept;

// Decodes a varint from src (up to len bytes). Stores result in *value.
// Returns number of bytes consumed, or 0 on error (truncated/overflow).
[[nodiscard]] size_t decode_varint(const uint8_t* src, size_t len, uint64_t* value) noexcept;

// Returns the number of bytes needed to encode value as a varint.
[[nodiscard]] size_t varint_length(uint64_t value) noexcept;

// Convenience: encode varint and append to string.
inline void encode_varint(uint64_t value, std::string* dst) {
    uint8_t buf[10];
    size_t n = encode_varint(value, buf);
    dst->append(reinterpret_cast<const char*>(buf), n);
}

// ZigZag encoding: maps signed integers to unsigned integers so that
// values with small absolute value have small varint encodings.
[[nodiscard]] inline uint64_t zigzag_encode(int64_t value) noexcept {
    return (static_cast<uint64_t>(value) << 1) ^ static_cast<uint64_t>(value >> 63);
}

[[nodiscard]] inline int64_t zigzag_decode(uint64_t value) noexcept {
    return static_cast<int64_t>((value >> 1) ^ -(value & 1));
}

} // namespace pl::sstv2::codec
