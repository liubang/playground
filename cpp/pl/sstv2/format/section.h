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
// Created: 2026/06/06 14:11

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "cpp/pl/sstv2/types/value.h"

namespace pl::sstv2::format {

enum class SectionMagic : uint32_t {
    kLocator = 0x41434F4C,       // LOCA
    kConfiguration = 0x47494643, // CFIG
    kSchema = 0x414D4553,        // SEMA
    kStatistics = 0x54415453,    // STAT
    kCompatibility = 0x504D4F43, // COMP
    kUserDefined = 0x52455355,   // USER
};

using SectionEntry = std::pair<std::string, types::Value>;
using SectionEntries = std::vector<SectionEntry>;
using SectionMap = types::Value;

struct Section {
    SectionMagic magic = SectionMagic::kLocator;
    uint64_t checksum = 0;
    SectionMap entries;
};

[[nodiscard]] std::string encode_section(SectionMagic magic, const SectionMap& entries);
[[nodiscard]] absl::StatusOr<Section> decode_section(std::string_view input, SectionMagic expected);
[[nodiscard]] SectionMap make_section_map(SectionEntries entries);
[[nodiscard]] const types::Value* find_section_value(const SectionMap& entries,
                                                     std::string_view key);

} // namespace pl::sstv2::format
