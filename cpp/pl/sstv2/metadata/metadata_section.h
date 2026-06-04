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
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"

namespace pl::sstv2::metadata {

class MetadataSection {
public:
    void put(std::string_view key, std::string_view value);
    void put_uint64(std::string_view key, uint64_t value);
    void put_uint32(std::string_view key, uint32_t value);
    void put_uint16(std::string_view key, uint16_t value);

    [[nodiscard]] std::optional<std::string_view> get(std::string_view key) const;
    [[nodiscard]] std::optional<uint64_t> get_uint64(std::string_view key) const;
    [[nodiscard]] std::optional<uint32_t> get_uint32(std::string_view key) const;
    [[nodiscard]] std::optional<uint16_t> get_uint16(std::string_view key) const;

    // Serialize with given section magic
    [[nodiscard]] std::string serialize(uint32_t section_magic) const;

    // Deserialize and verify magic + checksum
    static absl::StatusOr<MetadataSection> deserialize(std::span<const std::byte> data,
                                                       uint32_t expected_magic);

    [[nodiscard]] bool empty() const;
    [[nodiscard]] size_t size() const;

private:
    std::vector<std::pair<std::string, std::string>> entries_;
};

} // namespace pl::sstv2::metadata
