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

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"

namespace pl::sstv2::file {

struct SectionLocation {
    uint64_t offset = 0;
    uint64_t length = 0;
};

// PDF v0.95 locator: metadata-section shape, magic LOCA, checksum, map payload.
class Locator {
public:
    static constexpr uint32_t kMagic = 0x41434F4C; // "LOCA"

    void set_uint64(std::string key, uint64_t value);
    [[nodiscard]] std::optional<uint64_t> find_uint64(std::string_view key) const;

    void set_section(std::string_view name, SectionLocation location);
    [[nodiscard]] std::optional<SectionLocation> find_section(std::string_view name) const;

    [[nodiscard]] std::string encode() const;
    static absl::StatusOr<Locator> decode(std::span<const std::byte> bytes);

    [[nodiscard]] const std::map<std::string, uint64_t>& entries() const;

private:
    std::map<std::string, uint64_t> entries_;
};

} // namespace pl::sstv2::file
