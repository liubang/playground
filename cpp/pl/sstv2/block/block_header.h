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
// Created: 2026/06/04 13:06

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "absl/status/statusor.h"

namespace pl::sstv2::block {

struct BlockHeader {
    static constexpr size_t kSize = 52;
    static constexpr uint32_t kMagic = 0x53535432; // "SST2"

    uint32_t magic = kMagic;
    uint32_t checksum = 0;
    uint32_t uncompressed_size = 0;
    uint32_t compressed_size = 0;
    uint16_t num_rows = 0;
    uint16_t num_sub_columns = 0;
    uint8_t compression = 0;
    uint8_t reserved[3] = {};
    uint64_t first_row_key_offset = 0;
    uint64_t last_row_key_offset = 0;
    uint32_t data_table_size = 0;
    uint32_t column_store_size = 0;
    uint32_t offset_table_size = 0;

    // Serialize header to exactly kSize bytes
    void encode(std::byte* dst) const;

    // Deserialize header from exactly kSize bytes
    static absl::StatusOr<BlockHeader> decode(const std::byte* src);
};

} // namespace pl::sstv2::block
