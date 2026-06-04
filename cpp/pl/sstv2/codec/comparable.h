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

#include <cstdint>
#include <string>
#include <string_view>

namespace pl::sstv2::codec {

// Memcomparable encoding: produces byte sequences whose lexicographic
// (memcmp) order matches the logical value order.

// Unsigned integers: big-endian encoding.
void encode_uint32(uint32_t v, std::string* dst);
void encode_uint64(uint64_t v, std::string* dst);

// Signed integers: flip sign bit, then big-endian.
void encode_int32(int32_t v, std::string* dst);
void encode_int64(int64_t v, std::string* dst);

// Bytes/String: escaped encoding with 8-byte groups + continuation marker.
void encode_bytes(std::string_view data, std::string* dst);

// Descending variants: bitwise invert of the ascending encoding.
void encode_uint64_desc(uint64_t v, std::string* dst);
void encode_int64_desc(int64_t v, std::string* dst);
void encode_bytes_desc(std::string_view data, std::string* dst);

} // namespace pl::sstv2::codec
