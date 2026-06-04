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
// Created: 2026/06/04 22:27

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "absl/status/statusor.h"

namespace pl::sstv2::file {

// PDF v0.95 tail. It is always the final 32 bytes of a key file.
struct Tail {
    static constexpr size_t kSize = 32;
    static constexpr uint32_t kMagic = 0x00545353; // "SST\0", little-endian.
    static constexpr uint32_t kVersion = 2;

    uint64_t checksum = 0;
    uint64_t locator_offset = 0;
    uint64_t locator_length = 0;
    uint32_t version = kVersion;
    uint32_t magic = kMagic;

    [[nodiscard]] std::array<std::byte, kSize> encode() const;
    static absl::StatusOr<Tail> decode(std::span<const std::byte> bytes);
};

} // namespace pl::sstv2::file
