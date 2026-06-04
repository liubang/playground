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

#include "cpp/pl/sstv2/metadata/schema_meta.h"

#include <string>

#include "cpp/pl/sstv2/metadata/metadata_section.h"

namespace pl::sstv2::metadata {

namespace {
constexpr std::string_view kKeyColumnCount = "column_count";
// Per-column keys are "col_N_name" and "col_N_type"
std::string col_name_key(size_t idx) {
    return "col_" + std::to_string(idx) + "_name";
}
std::string col_type_key(size_t idx) {
    return "col_" + std::to_string(idx) + "_type";
}
} // namespace

std::string SchemaMeta::serialize() const {
    MetadataSection section;
    section.put_uint32(kKeyColumnCount, static_cast<uint32_t>(columns.size()));
    for (size_t i = 0; i < columns.size(); ++i) {
        section.put(col_name_key(i), columns[i].name);
        section.put_uint16(col_type_key(i), static_cast<uint16_t>(columns[i].data_type));
    }
    return section.serialize(kSchemaMagic);
}

absl::StatusOr<SchemaMeta> SchemaMeta::deserialize(std::span<const std::byte> data) {
    auto section_or = MetadataSection::deserialize(data, kSchemaMagic);
    if (!section_or.ok()) {
        return section_or.status();
    }
    auto& section = *section_or;

    SchemaMeta meta;
    auto count = section.get_uint32(kKeyColumnCount);
    if (!count) {
        return absl::InvalidArgumentError("schema meta missing column_count");
    }

    meta.columns.reserve(*count);
    for (uint32_t i = 0; i < *count; ++i) {
        ColumnDef col;
        auto name = section.get(col_name_key(i));
        if (!name) {
            return absl::InvalidArgumentError("schema meta missing column name");
        }
        col.name = std::string(*name);
        auto type = section.get_uint16(col_type_key(i));
        if (!type) {
            return absl::InvalidArgumentError("schema meta missing column type");
        }
        col.data_type = static_cast<uint8_t>(*type);
        meta.columns.push_back(std::move(col));
    }
    return meta;
}

} // namespace pl::sstv2::metadata
