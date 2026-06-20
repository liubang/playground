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
// Created: 2026/06/06 14:15

#include "cpp/pl/sstv2/format/metadata.h"

#include <memory>
#include <optional>
#include <string>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/sstv2/types/internal_schema.h"

namespace pl::sstv2::format {
namespace {

using types::DataType;
using types::SchemaBuilder;
using types::SortOrder;
using types::Value;

constexpr std::string_view kMaxEmbeddedValueSize = "MaxEmbeddedValueSizeInByte";
constexpr std::string_view kMaxDataBlockSoft = "MaxDataBlockSizeInByte_SoftLimit";
constexpr std::string_view kMaxDataBlockHard = "MaxDataBlockSizeInByte_HardLimit";
constexpr std::string_view kMaxDataBlockRows = "MaxDataBlockRowCount";
constexpr std::string_view kMaxIndexBlockSoft = "MaxIndexBlockSizeInByte_SoftLimit";
constexpr std::string_view kMaxIndexBlockHard = "MaxIndexBlockSizeInByte_HardLimit";
constexpr std::string_view kMaxIndexBlockRows = "MaxIndexBlockRowCount";

constexpr std::string_view kColumnCount = "ColumnCount";
constexpr std::string_view kRowKeyColumnCount = "RowKeyColumnCount";
constexpr std::string_view kChecksumKey = "ChecksumKey";
constexpr std::string_view kSplitKey = "SplitKey";
constexpr std::string_view kVersionKey = "VersionKey";
constexpr std::string_view kSystemKey = "SystemKey";
constexpr std::string_view kNonKey = "NonKey";
constexpr std::string_view kTotalRowCount = "TotalRowCount";
constexpr std::string_view kDataBlockCount = "DataBlockCount";
constexpr std::string_view kIndexBlockCount = "IndexBlockCount";
constexpr std::string_view kKeyFileSize = "KeyFileSize";
constexpr std::string_view kValueFileSize = "ValueFileSize";

std::string row_key_type_key(uint64_t index) {
    return absl::StrCat("RowKeyColumn", index, "_Type");
}

std::string row_key_order_key(uint64_t index) {
    return absl::StrCat("RowKeyColumn", index, "_Order");
}

std::string row_key_name_key(uint64_t index) {
    return absl::StrCat("RowKeyColumn", index, "_Name");
}

void add_entry(SectionEntries* entries, std::string_view key, Value value) {
    entries->emplace_back(std::string(key), std::move(value));
}

void add_entry(SectionEntries* entries, std::string key, Value value) {
    entries->emplace_back(std::move(key), std::move(value));
}

absl::StatusOr<uint64_t> require_uint64(const SectionMap& entries, std::string_view key) {
    const Value* value = find_section_value(entries, key);
    if (value == nullptr) {
        return absl::InvalidArgumentError(absl::StrCat("missing metadata key ", key));
    }
    if (value->type() != DataType::kUint64) {
        return absl::InvalidArgumentError(absl::StrCat("metadata key ", key, " must be Uint64"));
    }
    return value->as_uint64();
}

absl::StatusOr<uint64_t> optional_uint64(const SectionMap& entries,
                                         std::string_view key,
                                         uint64_t fallback) {
    const Value* value = find_section_value(entries, key);
    if (value == nullptr) {
        return fallback;
    }
    if (value->type() != DataType::kUint64) {
        return absl::InvalidArgumentError(absl::StrCat("metadata key ", key, " must be Uint64"));
    }
    return value->as_uint64();
}

absl::Status validate_optional_uint64(const SectionMap& entries,
                                      std::string_view key,
                                      uint64_t expected) {
    auto value = optional_uint64(entries, key, expected);
    if (!value.ok()) {
        return value.status();
    }
    if (*value != expected) {
        return absl::InvalidArgumentError(absl::StrCat("metadata key ", key, " mismatch"));
    }
    return absl::OkStatus();
}

absl::StatusOr<std::string> optional_string(const SectionMap& entries,
                                            std::string_view key,
                                            std::string fallback) {
    const Value* value = find_section_value(entries, key);
    if (value == nullptr) {
        return fallback;
    }
    if (value->type() != DataType::kString) {
        return absl::InvalidArgumentError(absl::StrCat("metadata key ", key, " must be String"));
    }
    return std::string(value->as_string());
}

} // namespace

SectionMap configuration_entries(const Configuration& configuration) {
    SectionEntries entries;
    add_entry(&entries,
              kMaxEmbeddedValueSize,
              Value::make<DataType::kUint64>(configuration.max_embedded_value_size));
    add_entry(&entries,
              kMaxDataBlockSoft,
              Value::make<DataType::kUint64>(configuration.max_data_block_size_soft_limit));
    add_entry(&entries,
              kMaxDataBlockHard,
              Value::make<DataType::kUint64>(configuration.max_data_block_size_hard_limit));
    add_entry(&entries,
              kMaxDataBlockRows,
              Value::make<DataType::kUint64>(configuration.max_data_block_row_count));
    add_entry(&entries,
              kMaxIndexBlockSoft,
              Value::make<DataType::kUint64>(configuration.max_index_block_size_soft_limit));
    add_entry(&entries,
              kMaxIndexBlockHard,
              Value::make<DataType::kUint64>(configuration.max_index_block_size_hard_limit));
    add_entry(&entries,
              kMaxIndexBlockRows,
              Value::make<DataType::kUint64>(configuration.max_index_block_row_count));
    return make_section_map(std::move(entries));
}

absl::StatusOr<Configuration> configuration_from_entries(const SectionMap& entries) {
    Configuration configuration;
    auto embedded = require_uint64(entries, kMaxEmbeddedValueSize);
    if (!embedded.ok()) {
        return embedded.status();
    }
    auto soft = require_uint64(entries, kMaxDataBlockSoft);
    if (!soft.ok()) {
        return soft.status();
    }
    auto hard = require_uint64(entries, kMaxDataBlockHard);
    if (!hard.ok()) {
        return hard.status();
    }
    auto rows = require_uint64(entries, kMaxDataBlockRows);
    if (!rows.ok()) {
        return rows.status();
    }
    auto index_soft =
        optional_uint64(entries, kMaxIndexBlockSoft, configuration.max_index_block_size_soft_limit);
    if (!index_soft.ok()) {
        return index_soft.status();
    }
    auto index_hard =
        optional_uint64(entries, kMaxIndexBlockHard, configuration.max_index_block_size_hard_limit);
    if (!index_hard.ok()) {
        return index_hard.status();
    }
    auto index_rows =
        optional_uint64(entries, kMaxIndexBlockRows, configuration.max_index_block_row_count);
    if (!index_rows.ok()) {
        return index_rows.status();
    }
    configuration.max_embedded_value_size = *embedded;
    configuration.max_data_block_size_soft_limit = *soft;
    configuration.max_data_block_size_hard_limit = *hard;
    configuration.max_data_block_row_count = *rows;
    configuration.max_index_block_size_soft_limit = *index_soft;
    configuration.max_index_block_size_hard_limit = *index_hard;
    configuration.max_index_block_row_count = *index_rows;
    return configuration;
}

SectionMap schema_entries(const types::Schema& schema) {
    SectionEntries entries;
    const uint64_t user_columns = schema.row_key_column_count();
    add_entry(
        &entries,
        kColumnCount,
        Value::make<DataType::kUint64>(user_columns + types::InternalSchema::kSystemColumnCount));
    add_entry(&entries, kRowKeyColumnCount, Value::make<DataType::kUint64>(user_columns));
    add_entry(&entries, kChecksumKey, Value::make<DataType::kBinary>(std::string{}));
    add_entry(&entries, kSplitKey, Value::make<DataType::kUint64>(uint64_t{0}));
    add_entry(&entries, kVersionKey, Value::make<DataType::kUint64>(user_columns));
    add_entry(&entries, kSystemKey, Value::make<DataType::kUint64>(uint64_t{2}));
    add_entry(&entries, kNonKey, Value::make<DataType::kUint64>(user_columns + 2));
    for (uint64_t i = 0; i < user_columns; ++i) {
        add_entry(&entries,
                  row_key_type_key(i),
                  Value::make<DataType::kUint64>(static_cast<uint8_t>(schema.column_type(i))));
        add_entry(&entries,
                  row_key_order_key(i),
                  Value::make<DataType::kUint64>(static_cast<uint8_t>(schema.column_order(i))));
        add_entry(
            &entries, row_key_name_key(i), Value::make<DataType::kString>(schema.column_name(i)));
    }
    return make_section_map(std::move(entries));
}

absl::StatusOr<types::Schema::ConstRef> schema_from_entries(const SectionMap& entries) {
    auto count = require_uint64(entries, kRowKeyColumnCount);
    if (!count.ok()) {
        return count.status();
    }
    auto column_count =
        optional_uint64(entries, kColumnCount, *count + types::InternalSchema::kSystemColumnCount);
    if (!column_count.ok()) {
        return column_count.status();
    }
    if (*column_count != *count + types::InternalSchema::kSystemColumnCount) {
        return absl::InvalidArgumentError("schema column count mismatch");
    }
    auto status = validate_optional_uint64(entries, kVersionKey, *count);
    if (!status.ok()) {
        return status;
    }
    status = validate_optional_uint64(entries, kSystemKey, 2);
    if (!status.ok()) {
        return status;
    }
    status = validate_optional_uint64(entries, kNonKey, *count + 2);
    if (!status.ok()) {
        return status;
    }

    SchemaBuilder builder;
    for (uint64_t i = 0; i < *count; ++i) {
        auto type = require_uint64(entries, row_key_type_key(i));
        if (!type.ok()) {
            return type.status();
        }
        auto order = require_uint64(entries, row_key_order_key(i));
        if (!order.ok()) {
            return order.status();
        }
        auto name = optional_string(entries, row_key_name_key(i), absl::StrCat("RowKey", i));
        if (!name.ok()) {
            return name.status();
        }

        builder.add_column(std::move(*name),
                           static_cast<DataType>(static_cast<uint8_t>(*type)),
                           static_cast<SortOrder>(static_cast<uint8_t>(*order)));
    }

    std::optional<types::Schema> schema = builder.build();
    if (!schema.has_value()) {
        return absl::InvalidArgumentError(builder.error());
    }
    return std::make_shared<const types::Schema>(std::move(*schema));
}

SectionMap statistics_entries(const Statistics& statistics) {
    SectionEntries entries;
    add_entry(&entries, kTotalRowCount, Value::make<DataType::kUint64>(statistics.total_row_count));
    add_entry(
        &entries, kDataBlockCount, Value::make<DataType::kUint64>(statistics.data_block_count));
    add_entry(
        &entries, kIndexBlockCount, Value::make<DataType::kUint64>(statistics.index_block_count));
    add_entry(&entries, kKeyFileSize, Value::make<DataType::kUint64>(statistics.key_file_size));
    add_entry(&entries, kValueFileSize, Value::make<DataType::kUint64>(statistics.value_file_size));
    return make_section_map(std::move(entries));
}

absl::StatusOr<Statistics> statistics_from_entries(const SectionMap& entries) {
    Statistics statistics;
    auto rows = require_uint64(entries, kTotalRowCount);
    if (!rows.ok()) {
        return rows.status();
    }
    auto data_blocks = require_uint64(entries, kDataBlockCount);
    if (!data_blocks.ok()) {
        return data_blocks.status();
    }
    auto index_blocks = require_uint64(entries, kIndexBlockCount);
    if (!index_blocks.ok()) {
        return index_blocks.status();
    }
    auto key_size = require_uint64(entries, kKeyFileSize);
    if (!key_size.ok()) {
        return key_size.status();
    }
    auto value_size = require_uint64(entries, kValueFileSize);
    if (!value_size.ok()) {
        return value_size.status();
    }
    statistics.total_row_count = *rows;
    statistics.data_block_count = *data_blocks;
    statistics.index_block_count = *index_blocks;
    statistics.key_file_size = *key_size;
    statistics.value_file_size = *value_size;
    return statistics;
}

std::string encode_configuration(const Configuration& configuration) {
    return encode_section(SectionMagic::kConfiguration, configuration_entries(configuration));
}

std::string encode_schema(const types::Schema& schema) {
    return encode_section(SectionMagic::kSchema, schema_entries(schema));
}

std::string encode_statistics(const Statistics& statistics) {
    return encode_section(SectionMagic::kStatistics, statistics_entries(statistics));
}

absl::StatusOr<Configuration> decode_configuration(std::string_view input) {
    auto section = decode_section(input, SectionMagic::kConfiguration);
    if (!section.ok()) {
        return section.status();
    }
    return configuration_from_entries(section->entries);
}

absl::StatusOr<types::Schema::ConstRef> decode_schema(std::string_view input) {
    auto section = decode_section(input, SectionMagic::kSchema);
    if (!section.ok()) {
        return section.status();
    }
    return schema_from_entries(section->entries);
}

absl::StatusOr<Statistics> decode_statistics(std::string_view input) {
    auto section = decode_section(input, SectionMagic::kStatistics);
    if (!section.ok()) {
        return section.status();
    }
    return statistics_from_entries(section->entries);
}

} // namespace pl::sstv2::format
