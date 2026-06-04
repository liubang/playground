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
// Created: 2026/06/04 15:23

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "absl/status/statusor.h"

namespace pl::sstv2::file {

struct Tail {
    static constexpr size_t kSize = 32;

    uint32_t magic = 0x53535432; // "SST2"
    uint16_t format_version = 1;
    uint16_t reserved = 0;
    uint64_t locator_offset = 0;
    uint32_t locator_size = 0;
    uint32_t locator_checksum = 0; // CRC32C of locator section
    uint64_t file_checksum = 0;    // CRC32C of entire file excluding tail

    void encode_to(std::byte* dst) const;
    static absl::StatusOr<Tail> decode_from(std::span<const std::byte> src);
};

} // namespace pl::sstv2::file
