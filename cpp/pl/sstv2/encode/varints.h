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
#include <span>
#include <utility>

namespace pl::sstv2::encode {

class Varints {
public:
    // Encode uint32/uint64 to dst buffer, return bytes written
    static size_t encode_uint32(uint32_t value, std::byte* dst);
    static size_t encode_uint64(uint64_t value, std::byte* dst);

    // Decode from src, return {value, bytes_consumed}
    static std::pair<uint32_t, size_t> decode_uint32(std::span<const std::byte> src);
    static std::pair<uint64_t, size_t> decode_uint64(std::span<const std::byte> src);

    // ZigZag encoding for signed integers
    static constexpr uint32_t zigzag_encode32(int32_t v) {
        return (static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31);
    }
    static constexpr uint64_t zigzag_encode64(int64_t v) {
        return (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63);
    }
    static constexpr int32_t zigzag_decode32(uint32_t v) {
        return static_cast<int32_t>((v >> 1) ^ -(v & 1));
    }
    static constexpr int64_t zigzag_decode64(uint64_t v) {
        return static_cast<int64_t>((v >> 1) ^ -(v & 1));
    }

    // Maximum encoded size
    static constexpr size_t kMaxVarint32Bytes = 5;
    static constexpr size_t kMaxVarint64Bytes = 10;
};

} // namespace pl::sstv2::encode
