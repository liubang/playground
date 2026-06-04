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

#include "cpp/pl/sstv2/codec/comparable.h"

#include <cstring>

namespace pl::sstv2::codec {

namespace {

// Encode a big-endian uint32.
void put_big_endian32(uint32_t v, std::string* dst) {
    char buf[4];
    buf[0] = static_cast<char>(v >> 24);
    buf[1] = static_cast<char>(v >> 16);
    buf[2] = static_cast<char>(v >> 8);
    buf[3] = static_cast<char>(v);
    dst->append(buf, 4);
}

// Encode a big-endian uint64.
void put_big_endian64(uint64_t v, std::string* dst) {
    char buf[8];
    buf[0] = static_cast<char>(v >> 56);
    buf[1] = static_cast<char>(v >> 48);
    buf[2] = static_cast<char>(v >> 40);
    buf[3] = static_cast<char>(v >> 32);
    buf[4] = static_cast<char>(v >> 24);
    buf[5] = static_cast<char>(v >> 16);
    buf[6] = static_cast<char>(v >> 8);
    buf[7] = static_cast<char>(v);
    dst->append(buf, 8);
}

// Bitwise invert the last n bytes of dst.
void invert_tail(std::string* dst, size_t n) {
    size_t start = dst->size() - n;
    for (size_t i = start; i < dst->size(); ++i) {
        (*dst)[i] = static_cast<char>(~static_cast<unsigned char>((*dst)[i]));
    }
}

} // namespace

void encode_uint32(uint32_t v, std::string* dst) { put_big_endian32(v, dst); }

void encode_uint64(uint64_t v, std::string* dst) { put_big_endian64(v, dst); }

void encode_int32(int32_t v, std::string* dst) {
    // Flip sign bit so that negative < positive in unsigned comparison.
    uint32_t u = static_cast<uint32_t>(v) ^ (uint32_t{1} << 31);
    put_big_endian32(u, dst);
}

void encode_int64(int64_t v, std::string* dst) {
    uint64_t u = static_cast<uint64_t>(v) ^ (uint64_t{1} << 63);
    put_big_endian64(u, dst);
}

void encode_bytes(std::string_view data, std::string* dst) {
    // Escaped encoding: split into 8-byte groups.
    // Each group: [8 data bytes][1 marker byte]
    // Non-final groups: marker = 0xFF
    // Final group: pad with 0x00 to 8 bytes, marker = number of real bytes (1-8)
    const size_t len = data.size();
    size_t offset = 0;

    while (offset < len) {
        size_t remaining = len - offset;
        if (remaining >= 8) {
            // Non-final or exactly-final chunk of 8 bytes.
            dst->append(data.data() + offset, 8);
            if (remaining == 8) {
                // This is the last chunk with exactly 8 real bytes.
                dst->push_back(static_cast<char>(8));
                return;
            }
            dst->push_back(static_cast<char>(0xFF));
            offset += 8;
        } else {
            // Final chunk with < 8 bytes: pad with zeros.
            dst->append(data.data() + offset, remaining);
            dst->append(8 - remaining, '\0');
            dst->push_back(static_cast<char>(remaining));
            return;
        }
    }

    // Empty input: one group of 8 zero bytes with marker=0.
    if (len == 0) {
        dst->append(8, '\0');
        dst->push_back(static_cast<char>(0));
    }
}

void encode_uint64_desc(uint64_t v, std::string* dst) {
    size_t start = dst->size();
    encode_uint64(v, dst);
    invert_tail(dst, dst->size() - start);
}

void encode_int64_desc(int64_t v, std::string* dst) {
    size_t start = dst->size();
    encode_int64(v, dst);
    invert_tail(dst, dst->size() - start);
}

void encode_bytes_desc(std::string_view data, std::string* dst) {
    size_t start = dst->size();
    encode_bytes(data, dst);
    invert_tail(dst, dst->size() - start);
}

} // namespace pl::sstv2::codec
