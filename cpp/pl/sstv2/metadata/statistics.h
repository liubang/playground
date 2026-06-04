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

constexpr uint32_t kStatisticsMagic = 0x53544154; // "STAT"

struct Statistics {
    uint64_t total_rows = 0;
    uint32_t total_data_blocks = 0;
    uint32_t total_index_blocks = 0;
    uint64_t raw_data_size = 0;
    uint64_t compressed_data_size = 0;
    std::string min_row_key;
    std::string max_row_key;
    uint64_t creation_time = 0; // microseconds

    [[nodiscard]] std::string serialize() const;
    static absl::StatusOr<Statistics> deserialize(std::span<const std::byte> data);
};

} // namespace pl::sstv2::metadata
