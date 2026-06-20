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

#include "cpp/pl/sstv2/codec/scalar_comparable.h"

#include <cstring>

namespace pl::sstv2::codec {

namespace {

// =============================================================================
// Big-endian helpers.
// =============================================================================

void put_be8(uint8_t v, std::string* dst) {
    dst->push_back(static_cast<char>(v));
}

void put_be16(uint16_t v, std::string* dst) {
    char buf[2];
    buf[0] = static_cast<char>(v >> 8);
    buf[1] = static_cast<char>(v);
    dst->append(buf, 2);
}

void put_be32(uint32_t v, std::string* dst) {
    char buf[4];
    buf[0] = static_cast<char>(v >> 24);
    buf[1] = static_cast<char>(v >> 16);
    buf[2] = static_cast<char>(v >> 8);
    buf[3] = static_cast<char>(v);
    dst->append(buf, 4);
}

void put_be64(uint64_t v, std::string* dst) {
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

uint8_t read_be8(const uint8_t* src) {
    return src[0];
}

uint16_t read_be16(const uint8_t* src) {
    return static_cast<uint16_t>((static_cast<uint16_t>(src[0]) << 8) |
                                 static_cast<uint16_t>(src[1]));
}

uint32_t read_be32(const uint8_t* src) {
    return (static_cast<uint32_t>(src[0]) << 24) | (static_cast<uint32_t>(src[1]) << 16) |
           (static_cast<uint32_t>(src[2]) << 8) | static_cast<uint32_t>(src[3]);
}

uint64_t read_be64(const uint8_t* src) {
    return (static_cast<uint64_t>(src[0]) << 56) | (static_cast<uint64_t>(src[1]) << 48) |
           (static_cast<uint64_t>(src[2]) << 40) | (static_cast<uint64_t>(src[3]) << 32) |
           (static_cast<uint64_t>(src[4]) << 24) | (static_cast<uint64_t>(src[5]) << 16) |
           (static_cast<uint64_t>(src[6]) << 8) | static_cast<uint64_t>(src[7]);
}

// =============================================================================
// IEEE 754 float/double to ordered-unsigned transform.
//
// For positive floats, the IEEE 754 bit pattern already sorts correctly as
// unsigned integers. For negative floats, we need to flip all bits.
// For positive floats, we only flip the sign bit.
// This produces a total order: -NaN < -Inf < ... < -0 < +0 < ... < +Inf < +NaN
// =============================================================================

uint32_t float_to_ordered(float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    if ((bits & (uint32_t{1} << 31)) != 0u) {
        // Negative: flip all bits.
        bits = ~bits;
    } else {
        // Positive: flip sign bit only.
        bits ^= (uint32_t{1} << 31);
    }
    return bits;
}

float ordered_to_float(uint32_t bits) {
    if ((bits & (uint32_t{1} << 31)) != 0u) {
        // Was positive: flip sign bit back.
        bits ^= (uint32_t{1} << 31);
    } else {
        // Was negative: flip all bits back.
        bits = ~bits;
    }
    float v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

uint64_t double_to_ordered(double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    if ((bits & (uint64_t{1} << 63)) != 0u) {
        bits = ~bits;
    } else {
        bits ^= (uint64_t{1} << 63);
    }
    return bits;
}

double ordered_to_double(uint64_t bits) {
    if ((bits & (uint64_t{1} << 63)) != 0u) {
        bits ^= (uint64_t{1} << 63);
    } else {
        bits = ~bits;
    }
    double v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

// =============================================================================
// Bitwise invert helpers.
// =============================================================================

void invert_tail(std::string* dst, size_t n) {
    size_t start = dst->size() - n;
    for (size_t i = start; i < dst->size(); ++i) {
        (*dst)[i] = static_cast<char>(~static_cast<unsigned char>((*dst)[i]));
    }
}

// Invert a buffer in-place.
void invert_buf(uint8_t* buf, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        buf[i] = static_cast<uint8_t>(~buf[i]);
    }
}

} // namespace

// =============================================================================
// Encode: unsigned integers.
// =============================================================================

void encode_uint8(uint8_t v, std::string* dst) {
    put_be8(v, dst);
}
void encode_uint16(uint16_t v, std::string* dst) {
    put_be16(v, dst);
}
void encode_uint32(uint32_t v, std::string* dst) {
    put_be32(v, dst);
}
void encode_uint64(uint64_t v, std::string* dst) {
    put_be64(v, dst);
}

// =============================================================================
// Encode: signed integers (flip sign bit).
// =============================================================================

void encode_int8(int8_t v, std::string* dst) {
    uint8_t u = static_cast<uint8_t>(v) ^ (uint8_t{1} << 7);
    put_be8(u, dst);
}

void encode_int16(int16_t v, std::string* dst) {
    uint16_t u = static_cast<uint16_t>(v) ^ (uint16_t{1} << 15);
    put_be16(u, dst);
}

void encode_int32(int32_t v, std::string* dst) {
    uint32_t u = static_cast<uint32_t>(v) ^ (uint32_t{1} << 31);
    put_be32(u, dst);
}

void encode_int64(int64_t v, std::string* dst) {
    uint64_t u = static_cast<uint64_t>(v) ^ (uint64_t{1} << 63);
    put_be64(u, dst);
}

// =============================================================================
// Encode: floating point.
// =============================================================================

void encode_float(float v, std::string* dst) {
    put_be32(float_to_ordered(v), dst);
}

void encode_double(double v, std::string* dst) {
    put_be64(double_to_ordered(v), dst);
}

// =============================================================================
// Encode: bytes (escaped 8-byte group encoding).
//
// Each group: [8 data bytes][1 marker byte]
// Non-final groups: marker = 0xFF
// Final group: pad with 0x00 to 8 bytes, marker = number of real bytes (0-8)
// Empty input: marker = 0 (8 zero bytes + 0x00)
// =============================================================================

void encode_bytes(std::string_view data, std::string* dst) {
    const size_t len = data.size();
    size_t offset = 0;

    while (offset < len) {
        size_t remaining = len - offset;
        if (remaining >= 8) {
            dst->append(data.data() + offset, 8);
            if (remaining == 8) {
                dst->push_back(static_cast<char>(8));
                return;
            }
            dst->push_back(static_cast<char>(0xFF));
            offset += 8;
        } else {
            dst->append(data.data() + offset, remaining);
            dst->append(8 - remaining, '\0');
            dst->push_back(static_cast<char>(remaining));
            return;
        }
    }

    // Empty input.
    dst->append(8, '\0');
    dst->push_back('\0');
}

// =============================================================================
// Encode: descending variants (bitwise invert of ascending).
// =============================================================================

void encode_uint8_desc(uint8_t v, std::string* dst) {
    size_t start = dst->size();
    encode_uint8(v, dst);
    invert_tail(dst, dst->size() - start);
}

void encode_uint16_desc(uint16_t v, std::string* dst) {
    size_t start = dst->size();
    encode_uint16(v, dst);
    invert_tail(dst, dst->size() - start);
}

void encode_uint32_desc(uint32_t v, std::string* dst) {
    size_t start = dst->size();
    encode_uint32(v, dst);
    invert_tail(dst, dst->size() - start);
}

void encode_uint64_desc(uint64_t v, std::string* dst) {
    size_t start = dst->size();
    encode_uint64(v, dst);
    invert_tail(dst, dst->size() - start);
}

void encode_int8_desc(int8_t v, std::string* dst) {
    size_t start = dst->size();
    encode_int8(v, dst);
    invert_tail(dst, dst->size() - start);
}

void encode_int16_desc(int16_t v, std::string* dst) {
    size_t start = dst->size();
    encode_int16(v, dst);
    invert_tail(dst, dst->size() - start);
}

void encode_int32_desc(int32_t v, std::string* dst) {
    size_t start = dst->size();
    encode_int32(v, dst);
    invert_tail(dst, dst->size() - start);
}

void encode_int64_desc(int64_t v, std::string* dst) {
    size_t start = dst->size();
    encode_int64(v, dst);
    invert_tail(dst, dst->size() - start);
}

void encode_float_desc(float v, std::string* dst) {
    size_t start = dst->size();
    encode_float(v, dst);
    invert_tail(dst, dst->size() - start);
}

void encode_double_desc(double v, std::string* dst) {
    size_t start = dst->size();
    encode_double(v, dst);
    invert_tail(dst, dst->size() - start);
}

void encode_bytes_desc(std::string_view data, std::string* dst) {
    size_t start = dst->size();
    encode_bytes(data, dst);
    invert_tail(dst, dst->size() - start);
}

// =============================================================================
// Decode: unsigned integers.
// =============================================================================

size_t decode_uint8(const uint8_t* src, size_t len, uint8_t* value) {
    if (len < 1) {
        return 0;
    }
    *value = read_be8(src);
    return 1;
}

size_t decode_uint16(const uint8_t* src, size_t len, uint16_t* value) {
    if (len < 2) {
        return 0;
    }
    *value = read_be16(src);
    return 2;
}

size_t decode_uint32(const uint8_t* src, size_t len, uint32_t* value) {
    if (len < 4) {
        return 0;
    }
    *value = read_be32(src);
    return 4;
}

size_t decode_uint64(const uint8_t* src, size_t len, uint64_t* value) {
    if (len < 8) {
        return 0;
    }
    *value = read_be64(src);
    return 8;
}

// =============================================================================
// Decode: signed integers (flip sign bit back).
// =============================================================================

size_t decode_int8(const uint8_t* src, size_t len, int8_t* value) {
    if (len < 1) {
        return 0;
    }
    uint8_t u = read_be8(src) ^ (uint8_t{1} << 7);
    *value = static_cast<int8_t>(u);
    return 1;
}

size_t decode_int16(const uint8_t* src, size_t len, int16_t* value) {
    if (len < 2) {
        return 0;
    }
    uint16_t u = read_be16(src) ^ (uint16_t{1} << 15);
    *value = static_cast<int16_t>(u);
    return 2;
}

size_t decode_int32(const uint8_t* src, size_t len, int32_t* value) {
    if (len < 4) {
        return 0;
    }
    uint32_t u = read_be32(src) ^ (uint32_t{1} << 31);
    *value = static_cast<int32_t>(u);
    return 4;
}

size_t decode_int64(const uint8_t* src, size_t len, int64_t* value) {
    if (len < 8) {
        return 0;
    }
    uint64_t u = read_be64(src) ^ (uint64_t{1} << 63);
    *value = static_cast<int64_t>(u);
    return 8;
}

// =============================================================================
// Decode: floating point.
// =============================================================================

size_t decode_float(const uint8_t* src, size_t len, float* value) {
    if (len < 4) {
        return 0;
    }
    uint32_t bits = read_be32(src);
    *value = ordered_to_float(bits);
    return 4;
}

size_t decode_double(const uint8_t* src, size_t len, double* value) {
    if (len < 8) {
        return 0;
    }
    uint64_t bits = read_be64(src);
    *value = ordered_to_double(bits);
    return 8;
}

// =============================================================================
// Decode: bytes (reverse the 8-byte group encoding).
// =============================================================================

size_t decode_bytes(const uint8_t* src, size_t len, std::string* value) {
    value->clear();
    size_t offset = 0;

    while (true) {
        // Each group is 9 bytes: 8 data + 1 marker.
        if (offset + 9 > len) {
            return 0;
        }

        uint8_t marker = src[offset + 8];
        if (marker == 0xFF) {
            // Non-final group: all 8 bytes are data.
            value->append(reinterpret_cast<const char*>(src + offset), 8);
            offset += 9;
        } else {
            // Final group: marker indicates number of real bytes (0-8).
            if (marker > 8)
                return 0; // invalid marker
            value->append(reinterpret_cast<const char*>(src + offset), marker);
            offset += 9;
            return offset;
        }
    }
}

// =============================================================================
// Decode: descending variants.
//
// Strategy: invert the bytes into a temp buffer, then decode as ascending.
// =============================================================================

size_t decode_uint8_desc(const uint8_t* src, size_t len, uint8_t* value) {
    if (len < 1) {
        return 0;
    }
    uint8_t buf[1] = {static_cast<uint8_t>(~src[0])};
    return decode_uint8(buf, 1, value);
}

size_t decode_uint16_desc(const uint8_t* src, size_t len, uint16_t* value) {
    if (len < 2) {
        return 0;
    }
    uint8_t buf[2];
    std::memcpy(buf, src, 2);
    invert_buf(buf, 2);
    return decode_uint16(buf, 2, value);
}

size_t decode_uint32_desc(const uint8_t* src, size_t len, uint32_t* value) {
    if (len < 4) {
        return 0;
    }
    uint8_t buf[4];
    std::memcpy(buf, src, 4);
    invert_buf(buf, 4);
    return decode_uint32(buf, 4, value);
}

size_t decode_uint64_desc(const uint8_t* src, size_t len, uint64_t* value) {
    if (len < 8) {
        return 0;
    }
    uint8_t buf[8];
    std::memcpy(buf, src, 8);
    invert_buf(buf, 8);
    return decode_uint64(buf, 8, value);
}

size_t decode_int8_desc(const uint8_t* src, size_t len, int8_t* value) {
    if (len < 1) {
        return 0;
    }
    uint8_t buf[1] = {static_cast<uint8_t>(~src[0])};
    return decode_int8(buf, 1, value);
}

size_t decode_int16_desc(const uint8_t* src, size_t len, int16_t* value) {
    if (len < 2) {
        return 0;
    }
    uint8_t buf[2];
    std::memcpy(buf, src, 2);
    invert_buf(buf, 2);
    return decode_int16(buf, 2, value);
}

size_t decode_int32_desc(const uint8_t* src, size_t len, int32_t* value) {
    if (len < 4) {
        return 0;
    }
    uint8_t buf[4];
    std::memcpy(buf, src, 4);
    invert_buf(buf, 4);
    return decode_int32(buf, 4, value);
}

size_t decode_int64_desc(const uint8_t* src, size_t len, int64_t* value) {
    if (len < 8) {
        return 0;
    }
    uint8_t buf[8];
    std::memcpy(buf, src, 8);
    invert_buf(buf, 8);
    return decode_int64(buf, 8, value);
}

size_t decode_float_desc(const uint8_t* src, size_t len, float* value) {
    if (len < 4) {
        return 0;
    }
    uint8_t buf[4];
    std::memcpy(buf, src, 4);
    invert_buf(buf, 4);
    return decode_float(buf, 4, value);
}

size_t decode_double_desc(const uint8_t* src, size_t len, double* value) {
    if (len < 8) {
        return 0;
    }
    uint8_t buf[8];
    std::memcpy(buf, src, 8);
    invert_buf(buf, 8);
    return decode_double(buf, 8, value);
}

size_t decode_bytes_desc(const uint8_t* src, size_t len, std::string* value) {
    // We need to find the end of the encoded bytes first.
    // Scan for the terminating group (marker != 0x00 after inversion, i.e., marker != 0xFF in src).
    // Strategy: copy all bytes, invert, then decode.
    // But we don't know the length upfront. Scan group by group.
    size_t offset = 0;
    while (true) {
        if (offset + 9 > len) {
            return 0;
        }
        // After inversion, marker 0xFF means non-final. In desc encoding,
        // the inverted 0xFF is 0x00. So in the src, 0x00 at position offset+8
        // means non-final group.
        auto inv_marker = static_cast<uint8_t>(~src[offset + 8]);
        if (inv_marker == 0xFF) {
            offset += 9;
        } else {
            offset += 9;
            break;
        }
    }

    // Now invert the entire range and decode.
    std::string tmp(reinterpret_cast<const char*>(src), offset);
    for (auto& c : tmp) {
        c = static_cast<char>(~static_cast<unsigned char>(c));
    }
    return decode_bytes(reinterpret_cast<const uint8_t*>(tmp.data()), tmp.size(), value);
}

} // namespace pl::sstv2::codec
