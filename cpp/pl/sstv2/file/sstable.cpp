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
#include "cpp/pl/sstv2/buffer/buffer_writer.h"
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

absl::Status ensure_sinks_valid(const Sinks& sinks) {
    if (sinks.filesystem == nullptr || sinks.key == io::kInvalidFileHandle ||
        sinks.value == io::kInvalidFileHandle) {
        return absl::InvalidArgumentError("builder sinks must not be null");
    }
    return absl::OkStatus();
}

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

absl::StatusOr<std::string> read_range(const std::shared_ptr<io::FileSystem>& filesystem,
                                       io::FileHandle file,
                                       uint64_t offset,
                                       uint64_t length) {
    if (filesystem == nullptr || file == io::kInvalidFileHandle) {
        return absl::InvalidArgumentError("file is invalid");
    }
    buffer::BufferWriter result(static_cast<size_t>(length));
    auto status =
        filesystem->read_at(file, offset, result.append_space(static_cast<size_t>(length)));
    if (!status.ok()) {
        return status;
    }
    return std::move(result).release();
}

absl::StatusOr<types::AllKeyView> all_key_view_for(const types::InternalSchema::ConstRef& schema,
                                                   const InternalRow& row) {
    return types::make_all_key_view(row, schema);
}

absl::StatusOr<Row> materialize_row(const types::InternalSchema::ConstRef& internal_schema,
                                    const block::BlockReader& block,
                                    size_t row_index,
                                    const std::shared_ptr<io::FileSystem>& filesystem,
                                    io::FileHandle value_file) {
    const auto& internal = block.rows()[row_index];
    std::vector<Value> all_key_cols;
    all_key_cols.reserve(internal_schema->sort_key_column_count());
    for (size_t i = 0; i < internal_schema->sort_key_column_count(); ++i) {
        all_key_cols.push_back(internal.columns[i]);
    }
    Row row = Row::from_all_key(types::AllKey::from_columns(std::move(all_key_cols)));
    const ColumnFlag value_flag = internal.flag(internal_schema);
    if (!value_flag.is_valid() || value_flag.is_index_entry()) {
        return absl::InvalidArgumentError("invalid value column flag");
    }
    const DataType value_type = value_flag.data_type();
    if (value_type == DataType::kNone) {
        return row;
    }
    if (value_type == DataType::kBool) {
        row.value = Value::make<DataType::kBool>(value_flag.bool_value());
        return row;
    }

    absl::StatusOr<std::string> value_bytes;
    if (internal.location(internal_schema) == types::ValueLocation::kEmbedded) {
        auto embedded = block.embedded_value(row_index, internal_schema);
        if (!embedded.ok()) {
            return embedded.status();
        }
        value_bytes = std::string(*embedded);
    } else {
        value_bytes = read_range(filesystem,
                                 value_file,
                                 internal.offset(internal_schema),
                                 internal.length(internal_schema));
    }
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
    const std::shared_ptr<io::FileSystem>& filesystem,
    io::FileHandle value_file,
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
        auto row = materialize_row(internal_schema, data, *row_index, filesystem, value_file);
        if (!row.ok()) {
            return row.status();
        }
        return std::optional<Row>{std::move(*row)};
    }
    return std::optional<Row>{};
}

absl::Status validate_statistics(const format::Statistics& statistics,
                                 uint64_t key_file_size,
                                 uint64_t value_file_size) {
    if (statistics.key_file_size != key_file_size) {
        return absl::InvalidArgumentError("statistics key file size mismatch");
    }
    if (statistics.value_file_size != value_file_size) {
        return absl::InvalidArgumentError("statistics value file size mismatch");
    }
    if (statistics.total_row_count > key_file_size) {
        return absl::InvalidArgumentError("statistics row count is not plausible");
    }
    return absl::OkStatus();
}

} // namespace

Builder::Builder(types::Schema::ConstRef schema, Sinks sinks, BuilderOptions options) noexcept
    : schema_(std::move(schema)), options_(std::move(options)), sinks_(std::move(sinks)) {
    try {
        internal_schema_ = schema_ == nullptr ? nullptr : InternalSchema::make(schema_);
        if (internal_schema_ != nullptr && sinks_.filesystem != nullptr &&
            sinks_.key != io::kInvalidFileHandle) {
            index_builder_ = std::make_unique<index::TreeBuilder>(
                internal_schema_,
                index_fanout(),
                options_.configuration.max_index_block_size_soft_limit,
                options_.configuration.max_index_block_size_hard_limit,
                options_.block_compression,
                sinks_.filesystem,
                sinks_.key);
        }
    } catch (const std::bad_alloc&) {
        initialization_status_ =
            absl::ResourceExhaustedError("SST builder initialization allocation failed");
    } catch (...) {
        initialization_status_ = absl::InternalError("SST builder initialization failed");
    }
}

Builder::~Builder() {
    if (sinks_.filesystem != nullptr) {
        if (sinks_.key != io::kInvalidFileHandle) {
            (void)sinks_.filesystem->close(sinks_.key);
        }
        if (sinks_.value != io::kInvalidFileHandle) {
            (void)sinks_.filesystem->close(sinks_.value);
        }
    }
}

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
    if (!initialization_status_.ok()) {
        return initialization_status_;
    }
    if (state_ != State::kOpen) {
        return absl::FailedPreconditionError("builder is not open");
    }
    if (schema_ == nullptr || internal_schema_ == nullptr) {
        return absl::InvalidArgumentError("schema is null");
    }
    if (index_builder_ == nullptr) {
        return absl::InvalidArgumentError("index builder is null");
    }
    auto status = ensure_sinks_valid(sinks_);
    if (!status.ok()) {
        return status;
    }
    status = validate_row(*schema_, row);
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
    uint64_t offset = 0;
    if (!embedded) {
        auto value_size = sinks_.filesystem->size(sinks_.value);
        if (!value_size.ok()) {
            return value_size.status();
        }
        offset = *value_size;
    }
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
        status = sinks_.filesystem->append(sinks_.value, std::as_bytes(std::span(encoded_payload)));
        if (!status.ok()) {
            return status;
        }
    }
    status = bloom_builder_.add(internal, internal_schema_);
    if (!status.ok()) {
        return status;
    }
    auto encoded_key = codec::make_encoded_all_key(internal, internal_schema_);
    if (!encoded_key.ok()) {
        state_ = State::kFailed;
        return encoded_key.status();
    }
    if (!min_key_.has_value()) {
        min_key_ = encoded_key->bytes();
    }
    max_key_ = encoded_key->bytes();
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
    if (index_builder_ == nullptr) {
        return absl::InvalidArgumentError("index builder is null");
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
    auto key_size = sinks_.filesystem->size(sinks_.key);
    if (!key_size.ok()) {
        return key_size.status();
    }
    const uint64_t offset = *key_size;
    const uint64_t length = encoded_block->size();
    status = sinks_.filesystem->append(sinks_.key, std::as_bytes(std::span(*encoded_block)));
    if (!status.ok()) {
        return status;
    }
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

absl::StatusOr<FinishResult> Builder::finish_result() {
    if (!initialization_status_.ok()) {
        return initialization_status_;
    }
    if (state_ == State::kFinished) {
        return *finish_result_;
    }
    if (state_ == State::kFailed) {
        return absl::FailedPreconditionError("builder is in a failed state");
    }
    state_ = State::kFailed;
    if (schema_ == nullptr || internal_schema_ == nullptr) {
        return absl::InvalidArgumentError("schema is null");
    }
    if (index_builder_ == nullptr) {
        return absl::InvalidArgumentError("index builder is null");
    }
    auto status = ensure_sinks_valid(sinks_);
    if (!status.ok()) {
        return status;
    }

    status = flush_data_block();
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
    auto key_size = sinks_.filesystem->size(sinks_.key);
    if (!key_size.ok()) {
        return key_size.status();
    }
    const uint64_t bloom_offset = *key_size;
    const uint64_t bloom_length = bloom.size();
    status = sinks_.filesystem->append(sinks_.key, std::as_bytes(std::span(bloom)));
    if (!status.ok()) {
        return status;
    }

    key_size = sinks_.filesystem->size(sinks_.key);
    if (!key_size.ok()) {
        return key_size.status();
    }
    const uint64_t configuration_offset = *key_size;
    const std::string configuration = format::encode_configuration(options_.configuration);
    status = sinks_.filesystem->append(sinks_.key, std::as_bytes(std::span(configuration)));
    if (!status.ok()) {
        return status;
    }
    key_size = sinks_.filesystem->size(sinks_.key);
    if (!key_size.ok()) {
        return key_size.status();
    }
    const uint64_t schema_offset = *key_size;
    const std::string schema = format::encode_schema(*schema_);
    status = sinks_.filesystem->append(sinks_.key, std::as_bytes(std::span(schema)));
    if (!status.ok()) {
        return status;
    }

    auto value_size = sinks_.filesystem->size(sinks_.value);
    if (!value_size.ok()) {
        return value_size.status();
    }
    format::Statistics statistics{
        .total_row_count = total_row_count_,
        .data_block_count = data_block_count_,
        .index_block_count = index->block_count,
        .key_file_size = 0,
        .value_file_size = *value_size,
    };
    key_size = sinks_.filesystem->size(sinks_.key);
    if (!key_size.ok()) {
        return key_size.status();
    }
    const uint64_t statistics_offset = *key_size;
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
        key_size = sinks_.filesystem->size(sinks_.key);
        if (!key_size.ok()) {
            return key_size.status();
        }
        const uint64_t key_file_size =
            *key_size + statistics_section.size() + locator.size() + format::Tail::kSize;
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

    status = sinks_.filesystem->append(sinks_.key, std::as_bytes(std::span(statistics_section)));
    if (!status.ok()) {
        return status;
    }
    key_size = sinks_.filesystem->size(sinks_.key);
    if (!key_size.ok()) {
        return key_size.status();
    }
    const uint64_t locator_offset = *key_size;
    status = sinks_.filesystem->append(sinks_.key, std::as_bytes(std::span(locator)));
    if (!status.ok()) {
        return status;
    }
    const std::string tail = format::encode_tail(
        format::Tail{.locator_offset = locator_offset, .locator_length = locator.size()});
    status = sinks_.filesystem->append(sinks_.key, std::as_bytes(std::span(tail)));
    if (!status.ok()) {
        return status;
    }

    auto key_identity = sinks_.filesystem->close(sinks_.key);
    sinks_.key = io::kInvalidFileHandle;
    if (!key_identity.ok()) {
        static_cast<void>(sinks_.filesystem->close(sinks_.value));
        sinks_.value = io::kInvalidFileHandle;
        return key_identity.status();
    }
    auto value_identity = sinks_.filesystem->close(sinks_.value);
    sinks_.value = io::kInvalidFileHandle;
    if (!value_identity.ok()) {
        return value_identity.status();
    }
    if (key_identity->length != statistics.key_file_size ||
        value_identity->length != statistics.value_file_size) {
        return absl::DataLossError("published file identity length does not match SST statistics");
    }
    finish_result_ = FinishResult{
        .key_file = *key_identity,
        .value_file = *value_identity,
        .row_count = total_row_count_,
        .sst_format_version = options_.configuration.sst_format_version,
        .key_format_version = options_.configuration.key_format_version,
        .row_key_schema_fingerprint = options_.configuration.row_key_schema_fingerprint,
        .comparator_domain_fingerprint =
            options_.configuration.comparator_domain_fingerprint,
        .checksum_algorithm = options_.configuration.checksum_algorithm,
        .min_key = min_key_,
        .max_key = max_key_,
    };
    state_ = State::kFinished;
    return *finish_result_;
}

absl::StatusOr<FinishResult> Builder::finish_to_sinks(types::Schema::ConstRef schema,
                                                      Sinks sinks,
                                                      BuilderOptions options,
                                                      const std::vector<types::Row>& rows) {
    Builder builder(std::move(schema), std::move(sinks), std::move(options));
    for (const auto& row : rows) {
        auto status = builder.add(row);
        if (!status.ok()) {
            return status;
        }
    }
    auto result = builder.finish_result();
    if (!result.ok()) {
        return result.status();
    }
    return result;
}

Reader::ReaderState::~ReaderState() {
    if (filesystem != nullptr) {
        if (key_file != io::kInvalidFileHandle) {
            (void)filesystem->close(key_file);
        }
        if (value_file != io::kInvalidFileHandle) {
            (void)filesystem->close(value_file);
        }
    }
}

absl::StatusOr<Reader> Reader::open(std::shared_ptr<io::FileSystem> filesystem,
                                    io::FileHandle key_file,
                                    io::FileHandle value_file) {
    if (filesystem == nullptr) {
        return absl::InvalidArgumentError("reader filesystem is null");
    }
    if (key_file == io::kInvalidFileHandle || value_file == io::kInvalidFileHandle) {
        if (key_file != io::kInvalidFileHandle) {
            (void)filesystem->close(key_file);
        }
        if (value_file != io::kInvalidFileHandle) {
            (void)filesystem->close(value_file);
        }
        return absl::InvalidArgumentError("reader files must not be null");
    }
    if (key_file == value_file) {
        (void)filesystem->close(key_file);
        return absl::InvalidArgumentError("reader key and value handles must be distinct");
    }
    std::shared_ptr<ReaderState> state;
    try {
        state = std::make_shared<ReaderState>();
    } catch (const std::bad_alloc&) {
        (void)filesystem->close(key_file);
        (void)filesystem->close(value_file);
        return absl::ResourceExhaustedError("reader state allocation failed");
    }
    state->filesystem = filesystem;
    state->key_file = key_file;
    state->value_file = value_file;

    auto key_file_size = filesystem->size(key_file);
    if (!key_file_size.ok()) {
        return key_file_size.status();
    }
    if (*key_file_size < format::Tail::kSize) {
        return absl::InvalidArgumentError("key file is shorter than tail");
    }

    auto tail_bytes =
        read_range(filesystem, key_file, *key_file_size - format::Tail::kSize, format::Tail::kSize);
    if (!tail_bytes.ok()) {
        return tail_bytes.status();
    }
    auto tail = format::decode_tail(*tail_bytes);
    if (!tail.ok()) {
        return tail.status();
    }

    auto locator_bytes =
        read_range(filesystem, key_file, tail->locator_offset, tail->locator_length);
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
    auto schema_bytes = read_range(filesystem, key_file, *schema_offset, *schema_length);
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
        read_range(filesystem, key_file, *configuration_offset, *configuration_length);
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
        read_range(filesystem, key_file, *statistics_offset, *statistics_length);
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

    auto value_file_size = filesystem->size(value_file);
    if (!value_file_size.ok()) {
        return value_file_size.status();
    }
    auto status = validate_statistics(*statistics, *key_file_size, *value_file_size);
    if (!status.ok()) {
        return status;
    }

    auto internal_schema = InternalSchema::make(*schema);
    auto root_bytes = read_range(filesystem, key_file, *root_offset, *root_length);
    if (!root_bytes.ok()) {
        return root_bytes.status();
    }
    auto root = block::BlockReader::open(*root_bytes, internal_schema, block::Kind::kRootIndex);
    if (!root.ok()) {
        return root.status();
    }

    auto bloom_bytes = read_range(filesystem, key_file, *bloom_offset, *bloom_length);
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

    state->schema = *schema;
    state->internal_schema = std::move(internal_schema);
    state->configuration = *configuration;
    state->statistics = *statistics;
    state->bloom = std::move(*bloom);
    state->root = index::BlockRef{.offset = *root_offset, .length = *root_length};

    Reader reader;
    reader.state_ = std::move(state);
    return reader;
}

absl::StatusOr<std::string> Reader::read_key_range(uint64_t offset, uint64_t length) const {
    if (state_ == nullptr) {
        return absl::FailedPreconditionError("reader state is null");
    }
    return read_range(state_->filesystem, state_->key_file, offset, length);
}

absl::StatusOr<std::string> Reader::read_value_range(uint64_t offset, uint64_t length) const {
    if (state_ == nullptr) {
        return absl::FailedPreconditionError("reader state is null");
    }
    return read_range(state_->filesystem, state_->value_file, offset, length);
}

absl::StatusOr<Reader::Iterator> Reader::new_iterator(const ScanOptions& options) const {
    if (state_ == nullptr) {
        return absl::FailedPreconditionError("reader state is null");
    }

    std::optional<types::PrefixKey> start_key;
    if (options.start.has_value()) {
        auto key = types::make_prefix_key(*options.start, state_->schema, state_->internal_schema);
        if (!key.ok()) {
            return key.status();
        }
        start_key = std::move(*key);
    }
    std::optional<types::PrefixKey> limit_key;
    if (options.limit.has_value()) {
        auto key = types::make_prefix_key(*options.limit, state_->schema, state_->internal_schema);
        if (!key.ok()) {
            return key.status();
        }
        limit_key = std::move(*key);
    }
    types::KeyComparator comparator(state_->internal_schema);
    if (start_key.has_value() && limit_key.has_value()) {
        auto less = comparator.prefix_less(*start_key, *limit_key);
        if (!less.ok()) {
            return less.status();
        }
        if (!*less) {
            return Iterator(state_, std::move(start_key), std::move(limit_key));
        }
    }
    return Iterator(state_, std::move(start_key), std::move(limit_key));
}

types::Schema::ConstRef Reader::schema() const {
    return state_ == nullptr ? nullptr : state_->schema;
}

const format::Configuration& Reader::configuration() const {
    static const format::Configuration kEmpty;
    return state_ == nullptr ? kEmpty : state_->configuration;
}

const format::Statistics& Reader::statistics() const {
    static const format::Statistics kEmpty;
    return state_ == nullptr ? kEmpty : state_->statistics;
}

absl::StatusOr<std::vector<Row>> Reader::scan() const {
    return scan(ScanOptions{});
}

absl::StatusOr<std::vector<Row>> Reader::scan(const ScanOptions& options) const {
    auto it = new_iterator(options);
    if (!it.ok()) {
        return it.status();
    }
    auto status = it->SeekToFirst();
    if (!status.ok()) {
        return status;
    }
    std::vector<Row> rows;
    while (it->Valid()) {
        rows.push_back(it->row());
        status = it->Next();
        if (!status.ok()) {
            return status;
        }
    }
    return rows;
}

absl::StatusOr<std::optional<Row>> Reader::get(const types::AllKey& all_key) const {
    if (state_ == nullptr) {
        return absl::FailedPreconditionError("reader state is null");
    }

    const auto rk = all_key.row_key_view();
    if (rk.column_count() != state_->schema->row_key_column_count()) {
        return absl::InvalidArgumentError("lookup key column count mismatch");
    }
    for (size_t i = 0; i < rk.column_count(); ++i) {
        if (rk.column(i).type() != state_->schema->column_type(i)) {
            return absl::InvalidArgumentError(
                absl::StrCat("lookup key column ", i, " type mismatch"));
        }
    }

    InternalRow probe = InternalRow::make(state_->internal_schema);
    const auto& all_key_cols = all_key.columns();
    for (size_t i = 0; i < state_->internal_schema->sort_key_column_count(); ++i) {
        probe.columns[i] = all_key_cols[i];
    }
    probe.columns[state_->internal_schema->flag_index()] =
        Value::make<DataType::kUint64>(ColumnFlag::for_value(DataType::kNone, false).raw());
    probe.columns[state_->internal_schema->filename_index()] = Value::make<DataType::kString>("");
    probe.columns[state_->internal_schema->offset_index()] =
        Value::make<DataType::kUint64>(uint64_t{0});
    probe.columns[state_->internal_schema->length_index()] =
        Value::make<DataType::kUint64>(uint64_t{0});
    probe.columns[state_->internal_schema->checksum_index()] =
        Value::make<DataType::kUint64>(uint64_t{0});

    auto encoded_target_key = codec::make_encoded_all_key(probe, state_->internal_schema);
    if (!encoded_target_key.ok()) {
        return encoded_target_key.status();
    }

    if (!state_->bloom.may_contain_all_key(*encoded_target_key)) {
        return std::optional<Row>{};
    }

    auto data_ref = index::TreeReader::find_data_block(
        state_->filesystem, state_->key_file, state_->internal_schema, state_->root, all_key);
    if (!data_ref.ok()) {
        return data_ref.status();
    }
    if (!data_ref->has_value()) {
        return std::optional<Row>{};
    }
    auto data_bytes = read_key_range((*data_ref)->offset, (*data_ref)->length);
    if (!data_bytes.ok()) {
        return data_bytes.status();
    }
    auto data = block::BlockReader::open(*data_bytes, state_->internal_schema, block::Kind::kData);
    if (!data.ok()) {
        return data.status();
    }
    return get_from_data_block(
        state_->filesystem, state_->value_file, state_->internal_schema, *data, all_key);
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

Reader::Iterator::Iterator(std::shared_ptr<const ReaderState> state,
                           std::optional<types::PrefixKey> start_key,
                           std::optional<types::PrefixKey> limit_key)
    : state_(std::move(state)),
      start_key_(std::move(start_key)),
      limit_key_(std::move(limit_key)),
      status_(absl::OkStatus()) {}

absl::Status Reader::Iterator::SeekToFirst() {
    return seek_impl(start_key_);
}

absl::Status Reader::Iterator::Seek(const KeyPrefix& target) {
    if (state_ == nullptr) {
        status_ = absl::FailedPreconditionError("iterator is detached");
        return status_;
    }
    auto key = types::make_prefix_key(target, state_->schema, state_->internal_schema);
    if (!key.ok()) {
        status_ = key.status();
        return status_;
    }
    return seek_impl(std::optional<types::PrefixKey>{std::move(*key)});
}

absl::Status Reader::Iterator::seek_impl(const std::optional<types::PrefixKey>& start_key) {
    if (state_ == nullptr) {
        status_ = absl::FailedPreconditionError("iterator is detached");
        return status_;
    }

    status_ = absl::OkStatus();
    valid_ = false;
    current_key_bytes_.clear();
    block_.reset();
    row_index_ = 0;

    auto cursor = index::ForwardCursor::open(state_->filesystem,
                                             state_->key_file,
                                             state_->internal_schema,
                                             state_->root,
                                             start_key,
                                             limit_key_);
    if (!cursor.ok()) {
        status_ = cursor.status();
        return status_;
    }
    cursor_ = std::move(*cursor);

    status_ = load_current_block(start_key, true);
    if (!status_.ok()) {
        return status_;
    }
    status_ = advance_to_next_valid();
    return status_;
}

absl::Status Reader::Iterator::load_current_block(const std::optional<types::PrefixKey>& start_key,
                                                  bool apply_start_key) {
    if (!cursor_.has_value()) {
        block_.reset();
        return absl::OkStatus();
    }

    bool apply_start = apply_start_key;
    while (cursor_->valid()) {
        const auto ref = cursor_->current();

        auto data_bytes = read_range(state_->filesystem, state_->key_file, ref.offset, ref.length);
        if (!data_bytes.ok()) {
            return data_bytes.status();
        }
        auto data =
            block::BlockReader::open(*data_bytes, state_->internal_schema, block::Kind::kData);
        if (!data.ok()) {
            return data.status();
        }

        block_ = std::move(*data);
        row_index_ = 0;
        if (apply_start && start_key.has_value()) {
            auto lower = lower_bound_by_key(state_->internal_schema, block_->rows(), *start_key);
            if (!lower.ok()) {
                return lower.status();
            }
            row_index_ = *lower;
        }
        if (row_index_ < block_->rows().size()) {
            return absl::OkStatus();
        }

        apply_start = false;
        auto status = cursor_->next();
        if (!status.ok()) {
            return status;
        }
    }
    if (!cursor_->status().ok()) {
        return cursor_->status();
    }
    block_.reset();
    return absl::OkStatus();
}

absl::Status Reader::Iterator::advance_to_next_valid() {
    if (state_ == nullptr) {
        return absl::FailedPreconditionError("iterator is detached");
    }

    types::KeyComparator comparator(state_->internal_schema);
    while (true) {
        if (!block_.has_value() || row_index_ >= block_->rows().size()) {
            if (cursor_.has_value() && cursor_->valid()) {
                auto status = cursor_->next();
                if (!status.ok()) {
                    valid_ = false;
                    return status;
                }
            }
            auto status = load_current_block(std::nullopt, false);
            if (!status.ok()) {
                valid_ = false;
                return status;
            }
            if (!block_.has_value() || row_index_ >= block_->rows().size()) {
                valid_ = false;
                return absl::OkStatus();
            }
        }

        auto row_all_key = all_key_view_for(state_->internal_schema, block_->rows()[row_index_]);
        if (!row_all_key.ok()) {
            valid_ = false;
            return row_all_key.status();
        }
        if (limit_key_.has_value()) {
            auto cmp = comparator.compare_all_key_to_prefix(*row_all_key, *limit_key_);
            if (!cmp.ok()) {
                valid_ = false;
                return cmp.status();
            }
            if (*cmp >= 0) {
                valid_ = false;
                return absl::OkStatus();
            }
        }

        auto row = materialize_row(
            state_->internal_schema, *block_, row_index_, state_->filesystem, state_->value_file);
        if (!row.ok()) {
            valid_ = false;
            return row.status();
        }
        auto encoded_key =
            codec::make_encoded_all_key(block_->rows()[row_index_], state_->internal_schema);
        if (!encoded_key.ok()) {
            valid_ = false;
            return encoded_key.status();
        }
        current_row_ = std::move(*row);
        current_key_bytes_ = std::string(encoded_key->bytes());
        valid_ = true;
        return absl::OkStatus();
    }
}

absl::Status Reader::Iterator::Next() {
    if (!status_.ok()) {
        return status_;
    }
    if (!valid_) {
        return status_;
    }
    ++row_index_;
    status_ = advance_to_next_valid();
    return status_;
}

size_t Reader::Iterator::state_slots_for_test() const {
    if (!cursor_.has_value()) {
        return 0;
    }
    return cursor_->state_slots_for_test();
}

} // namespace pl::sstv2::file
