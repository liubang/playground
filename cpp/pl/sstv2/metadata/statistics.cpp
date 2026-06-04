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

#include "cpp/pl/sstv2/metadata/statistics.h"

#include "cpp/pl/sstv2/metadata/metadata_section.h"

namespace pl::sstv2::metadata {

namespace {
constexpr std::string_view kKeyTotalRows = "total_rows";
constexpr std::string_view kKeyTotalDataBlocks = "total_data_blocks";
constexpr std::string_view kKeyTotalIndexBlocks = "total_index_blocks";
constexpr std::string_view kKeyRawDataSize = "raw_data_size";
constexpr std::string_view kKeyCompressedDataSize = "compressed_data_size";
constexpr std::string_view kKeyMinRowKey = "min_row_key";
constexpr std::string_view kKeyMaxRowKey = "max_row_key";
constexpr std::string_view kKeyCreationTime = "creation_time";
} // namespace

std::string Statistics::serialize() const {
    MetadataSection section;
    section.put_uint64(kKeyTotalRows, total_rows);
    section.put_uint32(kKeyTotalDataBlocks, total_data_blocks);
    section.put_uint32(kKeyTotalIndexBlocks, total_index_blocks);
    section.put_uint64(kKeyRawDataSize, raw_data_size);
    section.put_uint64(kKeyCompressedDataSize, compressed_data_size);
    section.put(kKeyMinRowKey, min_row_key);
    section.put(kKeyMaxRowKey, max_row_key);
    section.put_uint64(kKeyCreationTime, creation_time);
    return section.serialize(kStatisticsMagic);
}

absl::StatusOr<Statistics> Statistics::deserialize(std::span<const std::byte> data) {
    auto section_or = MetadataSection::deserialize(data, kStatisticsMagic);
    if (!section_or.ok()) {
        return section_or.status();
    }
    auto& section = *section_or;

    Statistics stats;
    if (auto v = section.get_uint64(kKeyTotalRows)) {
        stats.total_rows = *v;
    }
    if (auto v = section.get_uint32(kKeyTotalDataBlocks)) {
        stats.total_data_blocks = *v;
    }
    if (auto v = section.get_uint32(kKeyTotalIndexBlocks)) {
        stats.total_index_blocks = *v;
    }
    if (auto v = section.get_uint64(kKeyRawDataSize)) {
        stats.raw_data_size = *v;
    }
    if (auto v = section.get_uint64(kKeyCompressedDataSize)) {
        stats.compressed_data_size = *v;
    }
    if (auto v = section.get(kKeyMinRowKey)) {
        stats.min_row_key = std::string(*v);
    }
    if (auto v = section.get(kKeyMaxRowKey)) {
        stats.max_row_key = std::string(*v);
    }
    if (auto v = section.get_uint64(kKeyCreationTime)) {
        stats.creation_time = *v;
    }
    return stats;
}

} // namespace pl::sstv2::metadata
