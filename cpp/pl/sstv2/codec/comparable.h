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
#include <string_view>

namespace pl::sstv2::codec {

// =============================================================================
// Memcomparable encoding: produces byte sequences whose lexicographic
// (memcmp) order matches the logical value order.
//
// Encoding rules:
//   - Unsigned integers: big-endian.
//   - Signed integers: flip sign bit, then big-endian.
//   - Floating point: IEEE 754 sign-magnitude to ordered unsigned transform.
//   - Bytes/String: escaped 8-byte group encoding with continuation marker.
//   - Descending variants: bitwise invert of the ascending encoding.
// =============================================================================

// --- Unsigned integer encode ---
void encode_uint8(uint8_t v, std::string* dst);
void encode_uint16(uint16_t v, std::string* dst);
void encode_uint32(uint32_t v, std::string* dst);
void encode_uint64(uint64_t v, std::string* dst);

// --- Signed integer encode ---
void encode_int8(int8_t v, std::string* dst);
void encode_int16(int16_t v, std::string* dst);
void encode_int32(int32_t v, std::string* dst);
void encode_int64(int64_t v, std::string* dst);

// --- Floating point encode ---
void encode_float(float v, std::string* dst);
void encode_double(double v, std::string* dst);

// --- Bytes/String encode ---
void encode_bytes(std::string_view data, std::string* dst);

// --- Descending variants ---
void encode_uint8_desc(uint8_t v, std::string* dst);
void encode_uint16_desc(uint16_t v, std::string* dst);
void encode_uint32_desc(uint32_t v, std::string* dst);
void encode_uint64_desc(uint64_t v, std::string* dst);
void encode_int8_desc(int8_t v, std::string* dst);
void encode_int16_desc(int16_t v, std::string* dst);
void encode_int32_desc(int32_t v, std::string* dst);
void encode_int64_desc(int64_t v, std::string* dst);
void encode_float_desc(float v, std::string* dst);
void encode_double_desc(double v, std::string* dst);
void encode_bytes_desc(std::string_view data, std::string* dst);

// =============================================================================
// Decode functions.
//
// Each decode function reads from src[0..len), stores the decoded value in
// *value, and returns the number of bytes consumed. Returns 0 on error
// (insufficient data).
// =============================================================================

// --- Unsigned integer decode ---
size_t decode_uint8(const uint8_t* src, size_t len, uint8_t* value);
size_t decode_uint16(const uint8_t* src, size_t len, uint16_t* value);
size_t decode_uint32(const uint8_t* src, size_t len, uint32_t* value);
size_t decode_uint64(const uint8_t* src, size_t len, uint64_t* value);

// --- Signed integer decode ---
size_t decode_int8(const uint8_t* src, size_t len, int8_t* value);
size_t decode_int16(const uint8_t* src, size_t len, int16_t* value);
size_t decode_int32(const uint8_t* src, size_t len, int32_t* value);
size_t decode_int64(const uint8_t* src, size_t len, int64_t* value);

// --- Floating point decode ---
size_t decode_float(const uint8_t* src, size_t len, float* value);
size_t decode_double(const uint8_t* src, size_t len, double* value);

// --- Bytes decode ---
// Decodes into *value, returns bytes consumed from src. Returns 0 on error.
size_t decode_bytes(const uint8_t* src, size_t len, std::string* value);

// --- Descending decode ---
size_t decode_uint8_desc(const uint8_t* src, size_t len, uint8_t* value);
size_t decode_uint16_desc(const uint8_t* src, size_t len, uint16_t* value);
size_t decode_uint32_desc(const uint8_t* src, size_t len, uint32_t* value);
size_t decode_uint64_desc(const uint8_t* src, size_t len, uint64_t* value);
size_t decode_int8_desc(const uint8_t* src, size_t len, int8_t* value);
size_t decode_int16_desc(const uint8_t* src, size_t len, int16_t* value);
size_t decode_int32_desc(const uint8_t* src, size_t len, int32_t* value);
size_t decode_int64_desc(const uint8_t* src, size_t len, int64_t* value);
size_t decode_float_desc(const uint8_t* src, size_t len, float* value);
size_t decode_double_desc(const uint8_t* src, size_t len, double* value);
size_t decode_bytes_desc(const uint8_t* src, size_t len, std::string* value);

} // namespace pl::sstv2::codec
