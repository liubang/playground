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
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "absl/status/statusor.h"

namespace pl::sstv2::file {

// Section type identifiers
constexpr uint32_t kSectionDataBlocks = 0x01;
constexpr uint32_t kSectionIndexBlocks = 0x02;
constexpr uint32_t kSectionBloomFilter = 0x03;
constexpr uint32_t kSectionConfiguration = 0x10;
constexpr uint32_t kSectionSchema = 0x11;
constexpr uint32_t kSectionStatistics = 0x12;
constexpr uint32_t kSectionCompatibility = 0x13;
constexpr uint32_t kSectionUserDefined = 0x14;
constexpr uint32_t kSectionValueFile = 0x20;

struct LocatorEntry {
    uint32_t section_type = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
};

class Locator {
public:
    void add(uint32_t section_type, uint64_t offset, uint64_t size);
    std::optional<LocatorEntry> find(uint32_t section_type) const;

    // Serialize: [entry_count:varint][entries: type(4B) + offset(varint) + size(varint)]...
    std::string serialize() const;
    static absl::StatusOr<Locator> deserialize(std::span<const std::byte> data);

    const std::vector<LocatorEntry>& entries() const;

private:
    std::vector<LocatorEntry> entries_;
};

} // namespace pl::sstv2::file
