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
#include <string>

#include "absl/status/statusor.h"

namespace pl::sstv2::metadata {

constexpr uint32_t kConfigurationMagic = 0x434F4E46; // "CONF"

struct Configuration {
    uint32_t block_size = 65536;
    uint8_t compression = 0;
    uint32_t bloom_bits_per_key = 10;
    uint32_t value_size_threshold = 1024;
    uint8_t max_prefix_rounds = 4;

    [[nodiscard]] std::string serialize() const;
    static absl::StatusOr<Configuration> deserialize(std::span<const std::byte> data);
};

} // namespace pl::sstv2::metadata
