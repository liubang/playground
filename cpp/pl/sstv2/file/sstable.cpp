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
// Created: 2026/06/06 14:16

#include "cpp/pl/sstv2/file/sstable.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/sstv2/block/block.h"
#include "cpp/pl/sstv2/bloom/bloom.h"
#include "cpp/pl/sstv2/codec/fixed.h"
#include "cpp/pl/sstv2/codec/value_comparable.h"
#include "cpp/pl/sstv2/format/section.h"
#include "cpp/pl/sstv2/format/tail.h"
#include "cpp/pl/sstv2/index/index_tree.h"
#include "cpp/pl/sstv2/types/column_flag.h"
#include "cpp/pl/sstv2/types/internal_row.h"
#include "cpp/pl/sstv2/types/internal_schema.h"
#include "cpp/pl/sstv2/types/key_comparator.h"
#include "cpp/pl/sstv2/types/key_factory.h"
#include "cpp/pl/sstv2/types/value.h"

namespace pl::sstv2::file {
namespace {

using std::move;
using types::ColumnFlag;
using types::DataType;
using types::decode_value;
using types::encode_value;
using types::InternalRow;
using types::InternalSchema;
using types::OpType;
using types::Row;
using types::Value;

constexpr std::string_view kRootIndexOffset = "RootIndex_Offset";
constexpr std::string_view kRootIndexLength = "RootIndex_Length";
constexpr std::string_view kConfigurationOffset = "Configuration_Offset";
constexpr std::string_view kConfigurationLength = "Configuration_Length";
constexpr std::string_view kSchemaOffset = "Schema_Offset";
constexpr std::string_view kSchemaLength = "Schema_Length";
constexpr std::string_view kStatisticsOffset = "Statistics_Offset";
constexpr std::string_view kStatisticsLength = "Statistics_Length";
constexpr std::string_view kBloomOffset = "BloomFilter0_Offset";
constexpr std::string_view kBloomLength = "BloomFilter0_Length";

absl::StatusOr<uint64_t> locator_uint64(const format::SectionMap& entries, std::string_view key) {
    const Value* value = format::find_section_value(entries, key);
    if (value == nullptr) {
        return absl::InvalidArgumentError(absl::StrCat("missing locator key ", key));
    }
    if (value->type() != DataType::kUint64) {
        return absl::InvalidArgumentError(absl::StrCat("locator key ", key, " must be Uint64"));
    }
    return value->as_uint64();
}

void add_locator_entry(format::SectionEntries* entries, std::string_view key, uint64_t value) {
    entries->emplace_back(std::string(key), Value::make<DataType::kUint64>(value));
}

absl::Status validate_row(const types::Schema& schema, const Row& row) {
    const auto rk = row.row_key();
    if (rk.column_count() != schema.row_key_column_count()) {
        return absl::InvalidArgumentError("row key column count mismatch");
    }
    for (size_t i = 0; i < rk.column_count(); ++i) {
        if (rk.column(i).type() != schema.column_type(i)) {
            return absl::InvalidArgumentError(absl::StrCat("row key column ", i, " type mismatch"));
        }
    }
    return absl::OkStatus();
}

InternalRow make_internal_row(const types::InternalSchema::ConstRef& schema,
                              const Row& row,
                              std::string_view filename,
                              uint64_t value_offset,
                              uint64_t value_length,
                              uint64_t value_checksum) {
    InternalRow internal = InternalRow::make(schema);
    const auto& all_key_cols = row.all_key.columns();
    for (size_t i = 0; i < schema->sort_key_column_count(); ++i) {
        internal.columns[i] = all_key_cols[i];
    }
    const bool has_value = row.value.type() != DataType::kNone;
    const bool has_payload = has_value && row.value.type() != DataType::kBool;
    const bool bool_value = row.value.type() == DataType::kBool && row.value.as_bool();
    internal.columns[schema->flag_index()] = Value::make<DataType::kUint64>(
        ColumnFlag::for_value(row.value.type(), has_payload, bool_value).raw());
    internal.columns[schema->filename_index()] =
        Value::make<DataType::kString>(has_payload ? filename : std::string_view{});
    internal.columns[schema->offset_index()] = Value::make<DataType::kUint64>(value_offset);
    internal.columns[schema->length_index()] = Value::make<DataType::kUint64>(value_length);
    internal.columns[schema->checksum_index()] = Value::make<DataType::kUint64>(value_checksum);
    return internal;
}

absl::StatusOr<std::string_view> checked_slice(std::string_view bytes,
                                               uint64_t offset,
                                               uint64_t length,
                                               std::string_view label) {
    if (offset > bytes.size() || length > bytes.size() - offset) {
        return absl::InvalidArgumentError(absl::StrCat(label, " points outside file"));
    }
    return bytes.substr(static_cast<size_t>(offset), static_cast<size_t>(length));
}

absl::StatusOr<types::AllKeyView> all_key_view_for(const types::InternalSchema::ConstRef& schema,
                                                   const InternalRow& row) {
    return types::make_all_key_view(row, schema);
}

absl::StatusOr<Row> materialize_row(const types::InternalSchema::ConstRef& internal_schema,
                                    const block::BlockReader& block,
                                    size_t row_index,
                                    std::string_view value_file) {
    const auto& internal = block.rows()[row_index];
    std::vector<Value> all_key_cols;
    all_key_cols.reserve(internal_schema->sort_key_column_count());
    for (size_t i = 0; i < internal_schema->sort_key_column_count(); ++i) {
        all_key_cols.push_back(internal.columns[i]);
    }
    Row row = Row::from_all_key(types::AllKey::from_columns(std::move(all_key_cols)));
    const ColumnFlag value_flag = internal.flag(internal_schema);
    const DataType value_type = value_flag.data_type();
    if (value_type == DataType::kNone) {
        return row;
    }
    if (value_type == DataType::kBool) {
        row.value = Value::make<DataType::kBool>(value_flag.bool_value());
        return row;
    }

    absl::StatusOr<std::string_view> value_bytes =
        internal.location(internal_schema) == types::ValueLocation::kEmbedded
            ? block.embedded_value(row_index, internal_schema)
            : checked_slice(value_file,
                            internal.offset(internal_schema),
                            internal.length(internal_schema),
                            "value");
    if (!value_bytes.ok()) {
        return value_bytes.status();
    }
    if (value_flag.has_checksum() &&
        codec::crc32c_u64(*value_bytes) != internal.checksum(internal_schema)) {
        return absl::InvalidArgumentError("value checksum mismatch");
    }
    auto decoded = decode_value(value_type, *value_bytes);
    if (!decoded.ok()) {
        return decoded.status();
    }
    row.value = std::move(*decoded);
    return row;
}

template <typename Key>
absl::StatusOr<size_t> lower_bound_by_key(const types::InternalSchema::ConstRef& schema,
                                          const std::vector<InternalRow>& rows,
                                          const Key& target_key) {
    types::KeyComparator comparator(schema);
    size_t first = 0;
    size_t count = rows.size();
    while (count > 0) {
        const size_t step = count / 2;
        const size_t it = first + step;
        auto key = all_key_view_for(schema, rows[it]);
        if (!key.ok()) {
            return key.status();
        }
        absl::StatusOr<bool> less;
        if constexpr (std::is_same_v<typename Key::tag, types::PrefixKeyTag>) {
            less = comparator.all_key_less_than_prefix(*key, target_key);
        } else {
            less = comparator.all_key_less(*key, target_key);
        }
        if (!less.ok()) {
            return less.status();
        }
        if (*less) {
            first = it + 1;
            count -= step + 1;
        } else {
            count = step;
        }
    }
    return first;
}

absl::StatusOr<std::optional<Row>> get_from_data_block(
    std::string_view value_file,
    const types::InternalSchema::ConstRef& internal_schema,
    const block::BlockReader& data,
    const types::AllKey& target_key) {
    auto row_index = lower_bound_by_key(internal_schema, data.rows(), target_key);
    if (!row_index.ok()) {
        return row_index.status();
    }
    if (*row_index == data.rows().size()) {
        return std::optional<Row>{};
    }
    auto key = all_key_view_for(internal_schema, data.rows()[*row_index]);
    if (!key.ok()) {
        return key.status();
    }
    types::KeyComparator comparator(internal_schema);
    auto cmp = comparator.compare_all_key(*key, target_key);
    if (!cmp.ok()) {
        return cmp.status();
    }
    if (*cmp == 0) {
        auto row = materialize_row(internal_schema, data, *row_index, value_file);
        if (!row.ok()) {
            return row.status();
        }
        return std::optional<Row>{std::move(*row)};
    }
    return std::optional<Row>{};
}

absl::Status validate_statistics(const format::Statistics& statistics,
                                 std::string_view key_file,
                                 std::string_view value_file) {
    if (statistics.key_file_size != key_file.size()) {
        return absl::InvalidArgumentError("statistics key file size mismatch");
    }
    if (statistics.value_file_size != value_file.size()) {
        return absl::InvalidArgumentError("statistics value file size mismatch");
    }
    return absl::OkStatus();
}

} // namespace

Builder::Builder(types::Schema::ConstRef schema, BuilderOptions options)
    : schema_(std::move(schema)),
      internal_schema_(schema_ == nullptr ? nullptr : InternalSchema::make(schema_)),
      options_(std::move(options)),
      index_builder_(internal_schema_ == nullptr
                         ? nullptr
                         : std::make_unique<index::TreeBuilder>(
                               internal_schema_,
                               index_fanout(),
                               options_.configuration.max_index_block_size_soft_limit,
                               options_.configuration.max_index_block_size_hard_limit,
                               options_.block_compression,
                               &files_.key_file)) {}

uint64_t Builder::max_data_block_rows() const noexcept {
    return std::max<uint64_t>(1, options_.configuration.max_data_block_row_count);
}

uint64_t Builder::index_fanout() const noexcept {
    return std::max<uint64_t>(2, options_.configuration.max_index_block_row_count);
}

block::Options Builder::data_block_options() const noexcept {
    block::Options options;
    options.kind = block::Kind::kData;
    options.compression = options_.block_compression;
    options.max_block_size_soft_limit = std::max<uint64_t>(
        block::Header::kSize, options_.configuration.max_data_block_size_soft_limit);
    options.max_block_size_hard_limit = std::max<uint64_t>(
        options.max_block_size_soft_limit, options_.configuration.max_data_block_size_hard_limit);
    options.max_row_count = max_data_block_rows();
    return options;
}

absl::StatusOr<size_t> Builder::encoded_data_block_size_with(
    const InternalRow& candidate, std::string_view candidate_embedded) const {
    block::BlockBuilder builder(internal_schema_, data_block_options());
    for (size_t i = 0; i < pending_rows_.size(); ++i) {
        auto status = builder.add(pending_rows_[i], pending_embedded_values_[i]);
        if (!status.ok()) {
            return status;
        }
    }
    auto status = builder.add(candidate, std::string(candidate_embedded));
    if (!status.ok()) {
        return status;
    }
    auto encoded = builder.finish();
    if (!encoded.ok()) {
        return encoded.status();
    }
    return encoded->size();
}

absl::Status Builder::add(const Row& row) {
    if (finished_) {
        return absl::FailedPreconditionError("builder is already finished");
    }
    if (schema_ == nullptr || internal_schema_ == nullptr) {
        return absl::InvalidArgumentError("schema is null");
    }
    auto status = validate_row(*schema_, row);
    if (!status.ok()) {
        return status;
    }

    if (last_all_key_.has_value()) {
        types::KeyComparator comparator(internal_schema_);
        auto less = comparator.all_key_less(*last_all_key_, row.all_key);
        if (!less.ok()) {
            return less.status();
        }
        if (!*less) {
            return absl::InvalidArgumentError(
                "rows must be added in strictly increasing all-key order");
        }
    }

    const bool has_value = row.value.type() != DataType::kNone;
    const bool has_payload = has_value && row.value.type() != DataType::kBool;
    std::string encoded_payload;
    if (has_payload) {
        auto encoded_value = encode_value(row.value);
        if (!encoded_value.ok()) {
            return encoded_value.status();
        }
        encoded_payload = std::move(*encoded_value);
    }
    const bool embedded =
        has_payload && encoded_payload.size() <= options_.configuration.max_embedded_value_size;
    const uint64_t offset = embedded ? 0 : files_.value_file.size();
    const uint64_t length = encoded_payload.size();
    const uint64_t checksum = has_payload ? codec::crc32c_u64(encoded_payload) : 0;
    std::string embedded_value = embedded ? encoded_payload : std::string{};

    InternalRow internal = make_internal_row(internal_schema_,
                                             row,
                                             embedded ? types::kEmbeddedFilename
                                                      : std::string_view(options_.value_file_name),
                                             offset,
                                             length,
                                             checksum);

    const block::Options block_options = data_block_options();
    if (!pending_rows_.empty() && pending_rows_.size() >= block_options.max_row_count) {
        status = flush_data_block();
        if (!status.ok()) {
            return status;
        }
    }
    if (!pending_rows_.empty()) {
        auto candidate_size = encoded_data_block_size_with(internal, embedded_value);
        if (!candidate_size.ok()) {
            return candidate_size.status();
        }
        if (*candidate_size > block_options.max_block_size_soft_limit ||
            *candidate_size > block_options.max_block_size_hard_limit) {
            status = flush_data_block();
            if (!status.ok()) {
                return status;
            }
        }
    }
    if (pending_rows_.empty()) {
        auto single_size = encoded_data_block_size_with(internal, embedded_value);
        if (!single_size.ok()) {
            return single_size.status();
        }
        // PDF §6.1: a single row that exceeds the hard limit is allowed as an exception.
        // The block will contain only this one row, even though it exceeds the limit.
    }
    if (has_payload && !embedded) {
        files_.value_file.append(encoded_payload);
    }
    status = bloom_builder_.add(internal, internal_schema_);
    if (!status.ok()) {
        return status;
    }
    pending_rows_.push_back(std::move(internal));
    pending_embedded_values_.push_back(std::move(embedded_value));
    last_all_key_ = row.all_key;
    ++total_row_count_;
    if (pending_rows_.size() >= max_data_block_rows()) {
        return flush_data_block();
    }
    return absl::OkStatus();
}

absl::Status Builder::flush_data_block() {
    if (pending_rows_.empty()) {
        return absl::OkStatus();
    }
    auto status = index_builder_->prepare_for_data_block();
    if (!status.ok()) {
        return status;
    }

    block::BlockBuilder block_builder(internal_schema_, data_block_options());
    for (size_t i = 0; i < pending_rows_.size(); ++i) {
        status = block_builder.add(pending_rows_[i], std::move(pending_embedded_values_[i]));
        if (!status.ok()) {
            return status;
        }
    }
    auto encoded_block = block_builder.finish();
    if (!encoded_block.ok()) {
        return encoded_block.status();
    }
    const uint64_t offset = files_.key_file.size();
    const uint64_t length = encoded_block->size();
    files_.key_file.append(*encoded_block);
    ++data_block_count_;

    status = index_builder_->add_data_block(pending_rows_.back(),
                                            index::BlockRef{.offset = offset, .length = length},
                                            pending_rows_.size());
    if (!status.ok()) {
        return status;
    }
    pending_rows_.clear();
    pending_embedded_values_.clear();
    return absl::OkStatus();
}

absl::StatusOr<Files> Builder::finish() {
    if (finished_) {
        return absl::FailedPreconditionError("builder is already finished");
    }
    finished_ = true;
    if (schema_ == nullptr || internal_schema_ == nullptr) {
        return absl::InvalidArgumentError("schema is null");
    }

    auto status = flush_data_block();
    if (!status.ok()) {
        return status;
    }
    auto index = index_builder_->finish();
    if (!index.ok()) {
        return index.status();
    }
    const uint64_t root_offset = index->root.offset;
    const uint64_t root_length = index->root.length;

    std::string bloom = bloom_builder_.finish();
    const uint64_t bloom_offset = files_.key_file.size();
    const uint64_t bloom_length = bloom.size();
    files_.key_file.append(bloom);

    const uint64_t configuration_offset = files_.key_file.size();
    const std::string configuration = format::encode_configuration(options_.configuration);
    files_.key_file.append(configuration);
    const uint64_t schema_offset = files_.key_file.size();
    const std::string schema = format::encode_schema(*schema_);
    files_.key_file.append(schema);

    format::Statistics statistics{
        .total_row_count = total_row_count_,
        .data_block_count = data_block_count_,
        .index_block_count = index->block_count,
        .key_file_size = 0,
        .value_file_size = files_.value_file.size(),
    };
    const uint64_t statistics_offset = files_.key_file.size();
    std::string statistics_section = format::encode_statistics(statistics);

    format::SectionEntries locator_entries;
    add_locator_entry(&locator_entries, kRootIndexOffset, root_offset);
    add_locator_entry(&locator_entries, kRootIndexLength, root_length);
    add_locator_entry(&locator_entries, kConfigurationOffset, configuration_offset);
    add_locator_entry(&locator_entries, kConfigurationLength, configuration.size());
    add_locator_entry(&locator_entries, kSchemaOffset, schema_offset);
    add_locator_entry(&locator_entries, kSchemaLength, schema.size());
    add_locator_entry(&locator_entries, kStatisticsOffset, statistics_offset);
    add_locator_entry(&locator_entries, kStatisticsLength, statistics_section.size());
    add_locator_entry(&locator_entries, kBloomOffset, bloom_offset);
    add_locator_entry(&locator_entries, kBloomLength, bloom_length);

    std::string locator =
        format::encode_section(format::SectionMagic::kLocator,
                               format::make_section_map(format::SectionEntries(locator_entries)));
    // Fixed-point iteration to resolve statistics.key_file_size.
    // Monotonic convergence is guaranteed because key_file_size only grows
    // (never shrinks) as the statistics section serialization grows with
    // larger numbers. The 128-iteration cap is a safety valve.
    bool converged = false;
    for (int i = 0; i < 128; ++i) {
        for (auto& [key, value] : locator_entries) {
            if (key == kStatisticsLength) {
                value = Value::make<DataType::kUint64>(statistics_section.size());
                break;
            }
        }
        locator = format::encode_section(
            format::SectionMagic::kLocator,
            format::make_section_map(format::SectionEntries(locator_entries)));
        const uint64_t key_file_size = files_.key_file.size() + statistics_section.size() +
                                       locator.size() + format::Tail::kSize;
        if (statistics.key_file_size == key_file_size) {
            converged = true;
            break;
        }
        statistics.key_file_size = key_file_size;
        statistics_section = format::encode_statistics(statistics);
    }
    if (!converged) {
        return absl::InternalError("statistics section size failed to converge");
    }

    files_.key_file.append(statistics_section);
    const uint64_t locator_offset = files_.key_file.size();
    files_.key_file.append(locator);
    files_.key_file.append(format::encode_tail(
        format::Tail{.locator_offset = locator_offset, .locator_length = locator.size()}));
    return files_;
}

absl::StatusOr<Reader> Reader::open(std::string_view key_file, std::string_view value_file) {
    if (key_file.size() < format::Tail::kSize) {
        return absl::InvalidArgumentError("key file is shorter than tail");
    }
    auto tail = format::decode_tail(key_file.substr(key_file.size() - format::Tail::kSize));
    if (!tail.ok()) {
        return tail.status();
    }
    auto locator_bytes =
        checked_slice(key_file, tail->locator_offset, tail->locator_length, "locator");
    if (!locator_bytes.ok()) {
        return locator_bytes.status();
    }
    auto locator = format::decode_section(*locator_bytes, format::SectionMagic::kLocator);
    if (!locator.ok()) {
        return locator.status();
    }

    auto schema_offset = locator_uint64(locator->entries, kSchemaOffset);
    if (!schema_offset.ok()) {
        return schema_offset.status();
    }
    auto schema_length = locator_uint64(locator->entries, kSchemaLength);
    if (!schema_length.ok()) {
        return schema_length.status();
    }
    auto schema_bytes = checked_slice(key_file, *schema_offset, *schema_length, "schema");
    if (!schema_bytes.ok()) {
        return schema_bytes.status();
    }
    auto schema = format::decode_schema(*schema_bytes);
    if (!schema.ok()) {
        return schema.status();
    }

    auto configuration_offset = locator_uint64(locator->entries, kConfigurationOffset);
    if (!configuration_offset.ok()) {
        return configuration_offset.status();
    }
    auto configuration_length = locator_uint64(locator->entries, kConfigurationLength);
    if (!configuration_length.ok()) {
        return configuration_length.status();
    }
    auto configuration_bytes =
        checked_slice(key_file, *configuration_offset, *configuration_length, "configuration");
    if (!configuration_bytes.ok()) {
        return configuration_bytes.status();
    }
    auto configuration = format::decode_configuration(*configuration_bytes);
    if (!configuration.ok()) {
        return configuration.status();
    }

    auto statistics_offset = locator_uint64(locator->entries, kStatisticsOffset);
    if (!statistics_offset.ok()) {
        return statistics_offset.status();
    }
    auto statistics_length = locator_uint64(locator->entries, kStatisticsLength);
    if (!statistics_length.ok()) {
        return statistics_length.status();
    }
    auto statistics_bytes =
        checked_slice(key_file, *statistics_offset, *statistics_length, "statistics");
    if (!statistics_bytes.ok()) {
        return statistics_bytes.status();
    }
    auto statistics = format::decode_statistics(*statistics_bytes);
    if (!statistics.ok()) {
        return statistics.status();
    }

    auto root_offset = locator_uint64(locator->entries, kRootIndexOffset);
    if (!root_offset.ok()) {
        return root_offset.status();
    }
    auto root_length = locator_uint64(locator->entries, kRootIndexLength);
    if (!root_length.ok()) {
        return root_length.status();
    }
    auto bloom_offset = locator_uint64(locator->entries, kBloomOffset);
    if (!bloom_offset.ok()) {
        return bloom_offset.status();
    }
    auto bloom_length = locator_uint64(locator->entries, kBloomLength);
    if (!bloom_length.ok()) {
        return bloom_length.status();
    }

    auto status = validate_statistics(*statistics, key_file, value_file);
    if (!status.ok()) {
        return status;
    }

    auto internal_schema = InternalSchema::make(*schema);
    auto root_bytes = checked_slice(key_file, *root_offset, *root_length, "root index");
    if (!root_bytes.ok()) {
        return root_bytes.status();
    }
    auto root = block::BlockReader::open(*root_bytes, internal_schema, block::Kind::kRootIndex);
    if (!root.ok()) {
        return root.status();
    }

    auto bloom_bytes = checked_slice(key_file, *bloom_offset, *bloom_length, "bloom");
    if (!bloom_bytes.ok()) {
        return bloom_bytes.status();
    }
    auto bloom = bloom::Reader::open(*bloom_bytes);
    if (!bloom.ok()) {
        return bloom.status();
    }
    if (bloom->header().row_count != statistics->total_row_count) {
        return absl::InvalidArgumentError("bloom row count mismatch");
    }

    Reader reader;
    reader.schema_ = *schema;
    reader.internal_schema_ = std::move(internal_schema);
    reader.configuration_ = *configuration;
    reader.statistics_ = *statistics;
    reader.key_file_ = std::string(key_file);
    reader.value_file_ = std::string(value_file);
    reader.bloom_ = std::move(*bloom);
    reader.root_index_offset_ = *root_offset;
    reader.root_index_length_ = *root_length;
    return reader;
}

absl::StatusOr<std::vector<Row>> Reader::scan() const {
    return scan(ScanOptions{});
}

absl::StatusOr<std::vector<Row>> Reader::scan(const ScanOptions& options) const {
    std::optional<types::PrefixKey> start_key;
    if (options.start.has_value()) {
        auto key = types::make_prefix_key(*options.start, schema_, internal_schema_);
        if (!key.ok()) {
            return key.status();
        }
        start_key = std::move(*key);
    }
    std::optional<types::PrefixKey> limit_key;
    if (options.limit.has_value()) {
        auto key = types::make_prefix_key(*options.limit, schema_, internal_schema_);
        if (!key.ok()) {
            return key.status();
        }
        limit_key = std::move(*key);
    }
    types::KeyComparator comparator(internal_schema_);
    if (start_key.has_value() && limit_key.has_value()) {
        auto less = comparator.prefix_less(*start_key, *limit_key);
        if (!less.ok()) {
            return less.status();
        }
        if (!*less) {
            return std::vector<Row>{};
        }
    }

    std::vector<index::BlockRef> data_blocks;
    absl::Status status =
        start_key.has_value() || limit_key.has_value()
            ? index::TreeReader::scan_data_blocks_in_range(
                  key_file_,
                  internal_schema_,
                  index::BlockRef{.offset = root_index_offset_, .length = root_index_length_},
                  start_key,
                  limit_key,
                  &data_blocks)
            : index::TreeReader::scan_data_blocks(
                  key_file_,
                  internal_schema_,
                  index::BlockRef{.offset = root_index_offset_, .length = root_index_length_},
                  &data_blocks);
    if (!status.ok()) {
        return status;
    }

    std::vector<Row> rows;
    rows.reserve(static_cast<size_t>(statistics_.total_row_count));
    for (const index::BlockRef& ref : data_blocks) {
        auto data_bytes = checked_slice(key_file_, ref.offset, ref.length, "data block");
        if (!data_bytes.ok()) {
            return data_bytes.status();
        }
        auto data = block::BlockReader::open(*data_bytes, internal_schema_, block::Kind::kData);
        if (!data.ok()) {
            return data.status();
        }
        size_t row_index = 0;
        if (start_key.has_value()) {
            auto lower = lower_bound_by_key(internal_schema_, data->rows(), *start_key);
            if (!lower.ok()) {
                return lower.status();
            }
            row_index = *lower;
        }
        for (; row_index < data->rows().size(); ++row_index) {
            auto row_all_key = all_key_view_for(internal_schema_, data->rows()[row_index]);
            if (!row_all_key.ok()) {
                return row_all_key.status();
            }
            if (limit_key.has_value()) {
                auto cmp = comparator.compare_all_key_to_prefix(*row_all_key, *limit_key);
                if (!cmp.ok()) {
                    return cmp.status();
                }
                if (*cmp >= 0) {
                    return rows;
                }
            }
            auto row = materialize_row(internal_schema_, *data, row_index, value_file_);
            if (!row.ok()) {
                return row.status();
            }
            rows.push_back(std::move(*row));
        }
    }
    return rows;
}

absl::StatusOr<std::optional<Row>> Reader::get(const types::AllKey& all_key) const {
    // Validate row key portion against schema.
    const auto rk = all_key.row_key_view();
    if (rk.column_count() != schema_->row_key_column_count()) {
        return absl::InvalidArgumentError("lookup key column count mismatch");
    }
    for (size_t i = 0; i < rk.column_count(); ++i) {
        if (rk.column(i).type() != schema_->column_type(i)) {
            return absl::InvalidArgumentError(
                absl::StrCat("lookup key column ", i, " type mismatch"));
        }
    }

    // Build a probe InternalRow for bloom filter + encoded key generation.
    InternalRow probe = InternalRow::make(internal_schema_);
    const auto& all_key_cols = all_key.columns();
    for (size_t i = 0; i < internal_schema_->sort_key_column_count(); ++i) {
        probe.columns[i] = all_key_cols[i];
    }
    probe.columns[internal_schema_->flag_index()] =
        Value::make<DataType::kUint64>(ColumnFlag::for_value(DataType::kNone, false).raw());
    probe.columns[internal_schema_->filename_index()] = Value::make<DataType::kString>("");
    probe.columns[internal_schema_->offset_index()] = Value::make<DataType::kUint64>(uint64_t{0});
    probe.columns[internal_schema_->length_index()] = Value::make<DataType::kUint64>(uint64_t{0});
    probe.columns[internal_schema_->checksum_index()] = Value::make<DataType::kUint64>(uint64_t{0});

    auto encoded_target_key = codec::make_encoded_all_key(probe, internal_schema_);
    if (!encoded_target_key.ok()) {
        return encoded_target_key.status();
    }

    if (!bloom_.may_contain_all_key(*encoded_target_key)) {
        return std::optional<Row>{};
    }

    auto data_ref = index::TreeReader::find_data_block(
        key_file_,
        internal_schema_,
        index::BlockRef{.offset = root_index_offset_, .length = root_index_length_},
        all_key);
    if (!data_ref.ok()) {
        return data_ref.status();
    }
    if (!data_ref->has_value()) {
        return std::optional<Row>{};
    }
    auto data_bytes =
        checked_slice(key_file_, (*data_ref)->offset, (*data_ref)->length, "data block");
    if (!data_bytes.ok()) {
        return data_bytes.status();
    }
    auto data = block::BlockReader::open(*data_bytes, internal_schema_, block::Kind::kData);
    if (!data.ok()) {
        return data.status();
    }
    return get_from_data_block(value_file_, internal_schema_, *data, all_key);
}

absl::StatusOr<std::optional<Row>> Reader::get(const types::RowKey& row_key,
                                               types::SystemKey system_key) const {
    std::vector<Value> all_key_cols;
    all_key_cols.reserve(row_key.column_count() + types::SystemKey::kColumnCount);
    for (size_t i = 0; i < row_key.column_count(); ++i) {
        all_key_cols.push_back(row_key.column(i));
    }
    all_key_cols.push_back(Value::make<DataType::kVersion>(system_key.version));
    all_key_cols.push_back(Value::make<DataType::kUint8>(static_cast<uint8_t>(system_key.op_type)));
    return get(types::AllKey::from_columns(std::move(all_key_cols)));
}

} // namespace pl::sstv2::file
