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

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace pl::sstv2::codec {

// Canonical variable-width unsigned integer whose bytewise order is numeric order.
// Zero is encoded as 0x00. Non-zero values are encoded as a one-byte payload length
// followed by the shortest big-endian representation.
void encode_ordered_uint32(uint32_t value, std::string* dst);

// Decodes one canonical ordered uint32. Returns bytes consumed, or zero for malformed,
// truncated, non-canonical, or overflowing input.
[[nodiscard]] size_t decode_ordered_uint32(const uint8_t* src,
                                           size_t len,
                                           uint32_t* value) noexcept;

} // namespace pl::sstv2::codec
