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
// Created: 2026/07/17 22:27

#include "cpp/pl/minitable/codec/cell_key_codec.h"

#include <cmath>
#include <type_traits>

#include "absl/strings/str_cat.h"
#include "cpp/pl/sstv2/codec/fixed.h"
#include "cpp/pl/sstv2/codec/ordered_uint.h"
#include "cpp/pl/sstv2/codec/scalar_comparable.h"
#include "cpp/pl/sstv2/codec/value_comparable.h"

namespace pl::minitable::codec {
namespace {

using sstv2::types::DataType;
using sstv2::types::OpType;
using sstv2::types::Value;

constexpr uint8_t kRowTombstonePrefix = 0x00;
constexpr uint8_t kColumnFamilyTombstonePrefix = 0x01;
constexpr uint8_t kCellPrefix = 0x02;
constexpr uint8_t kStaticQualifierPrefix = 0x00;
constexpr uint8_t kDynamicQualifierPrefix = 0x01;

bool IsSupportedRowKeyType(DataType type) {
    switch (type) {
        case DataType::kBool:
        case DataType::kInt32:
        case DataType::kUint32:
        case DataType::kInt64:
        case DataType::kUint64:
        case DataType::kFloat:
        case DataType::kDouble:
        case DataType::kString:
        case DataType::kBinary:
            return true;
        default:
            return false;
    }
}

bool IsKnownOpType(OpType op_type) {
    return op_type == OpType::kPut || op_type == OpType::kMerge || op_type == OpType::kDelete;
}

struct DecodeCursor {
    std::string_view input;
    size_t offset = 0;

    [[nodiscard]] size_t remaining() const { return input.size() - offset; }
    [[nodiscard]] const uint8_t* data() const {
        return reinterpret_cast<const uint8_t*>(input.data() + offset);
    }
    bool Consume(size_t count) {
        if (count > remaining()) {
            return false;
        }
        offset += count;
        return true;
    }
};

absl::Status DataLoss(std::string_view message) {
    return absl::DataLossError(message);
}

absl::StatusOr<Value> DecodeRowKeyValue(DataType type,
                                        sstv2::types::SortOrder order,
                                        DecodeCursor* cursor) {
    const bool descending = order == sstv2::types::SortOrder::kDescending;
    size_t consumed = 0;
    switch (type) {
        case DataType::kBool: {
            uint8_t value = 0;
            consumed =
                descending
                    ? sstv2::codec::decode_uint8_desc(cursor->data(), cursor->remaining(), &value)
                    : sstv2::codec::decode_uint8(cursor->data(), cursor->remaining(), &value);
            if (consumed == 0 || value > 1) {
                return DataLoss("invalid boolean row-key component");
            }
            cursor->Consume(consumed);
            return Value::make<DataType::kBool>(value != 0);
        }
        case DataType::kInt32: {
            int32_t value = 0;
            consumed =
                descending
                    ? sstv2::codec::decode_int32_desc(cursor->data(), cursor->remaining(), &value)
                    : sstv2::codec::decode_int32(cursor->data(), cursor->remaining(), &value);
            if (consumed == 0)
                return DataLoss("truncated int32 row-key component");
            cursor->Consume(consumed);
            return Value::make<DataType::kInt32>(value);
        }
        case DataType::kUint32: {
            uint32_t value = 0;
            consumed =
                descending
                    ? sstv2::codec::decode_uint32_desc(cursor->data(), cursor->remaining(), &value)
                    : sstv2::codec::decode_uint32(cursor->data(), cursor->remaining(), &value);
            if (consumed == 0)
                return DataLoss("truncated uint32 row-key component");
            cursor->Consume(consumed);
            return Value::make<DataType::kUint32>(value);
        }
        case DataType::kInt64: {
            int64_t value = 0;
            consumed =
                descending
                    ? sstv2::codec::decode_int64_desc(cursor->data(), cursor->remaining(), &value)
                    : sstv2::codec::decode_int64(cursor->data(), cursor->remaining(), &value);
            if (consumed == 0)
                return DataLoss("truncated int64 row-key component");
            cursor->Consume(consumed);
            return Value::make<DataType::kInt64>(value);
        }
        case DataType::kUint64: {
            uint64_t value = 0;
            consumed =
                descending
                    ? sstv2::codec::decode_uint64_desc(cursor->data(), cursor->remaining(), &value)
                    : sstv2::codec::decode_uint64(cursor->data(), cursor->remaining(), &value);
            if (consumed == 0)
                return DataLoss("truncated uint64 row-key component");
            cursor->Consume(consumed);
            return Value::make<DataType::kUint64>(value);
        }
        case DataType::kFloat: {
            float value = 0;
            consumed =
                descending
                    ? sstv2::codec::decode_float_desc(cursor->data(), cursor->remaining(), &value)
                    : sstv2::codec::decode_float(cursor->data(), cursor->remaining(), &value);
            if (consumed == 0 || std::isnan(value) || (value == 0.0F && std::signbit(value))) {
                return DataLoss("non-canonical float row-key component");
            }
            cursor->Consume(consumed);
            return Value::make<DataType::kFloat>(value);
        }
        case DataType::kDouble: {
            double value = 0;
            consumed =
                descending
                    ? sstv2::codec::decode_double_desc(cursor->data(), cursor->remaining(), &value)
                    : sstv2::codec::decode_double(cursor->data(), cursor->remaining(), &value);
            if (consumed == 0 || std::isnan(value) || (value == 0.0 && std::signbit(value))) {
                return DataLoss("non-canonical double row-key component");
            }
            cursor->Consume(consumed);
            return Value::make<DataType::kDouble>(value);
        }
        case DataType::kString:
        case DataType::kBinary: {
            std::string value;
            consumed =
                descending
                    ? sstv2::codec::decode_bytes_desc(cursor->data(), cursor->remaining(), &value)
                    : sstv2::codec::decode_bytes(cursor->data(), cursor->remaining(), &value);
            if (consumed == 0)
                return DataLoss("invalid bytes row-key component");
            cursor->Consume(consumed);
            return type == DataType::kString ? Value::make<DataType::kString>(std::move(value))
                                             : Value::make<DataType::kBinary>(std::move(value));
        }
        default:
            return DataLoss("unsupported row-key type in persisted schema");
    }
}

} // namespace

absl::StatusOr<CellKeyCodec> CellKeyCodec::Create(KeyFormat format,
                                                  sstv2::types::Schema::ConstRef row_key_schema) {
    if (row_key_schema == nullptr) {
        return absl::InvalidArgumentError("row key schema is null");
    }
    if (format.version != KeyFormat::kCurrentVersion) {
        return absl::FailedPreconditionError("unsupported key format version");
    }
    if (format.partition_mode == PartitionMode::kHash) {
        if (format.hash_algorithm != HashAlgorithm::kXxh3_64V1 ||
            format.virtual_bucket_count == 0) {
            return absl::InvalidArgumentError(
                "HASH key format requires a hash algorithm and virtual buckets");
        }
    } else if (format.partition_mode == PartitionMode::kGlobalOrder) {
        if (format.hash_algorithm != HashAlgorithm::kNone || format.virtual_bucket_count != 0) {
            return absl::InvalidArgumentError(
                "GLOBAL_ORDER key format must not define hash settings");
        }
    } else {
        return absl::InvalidArgumentError("unknown partition mode");
    }
    if (row_key_schema->row_key_column_count() == 0 ||
        row_key_schema->row_key_column_count() > format.max_row_key_columns) {
        return absl::InvalidArgumentError("row key column count is outside format limits");
    }
    if (format.max_encoded_key_bytes == 0 || format.max_dynamic_qualifier_bytes == 0) {
        return absl::InvalidArgumentError("key format limits must be non-zero");
    }
    for (size_t i = 0; i < row_key_schema->row_key_column_count(); ++i) {
        const auto order = row_key_schema->column_order(i);
        if (order != sstv2::types::SortOrder::kAscending &&
            order != sstv2::types::SortOrder::kDescending) {
            return absl::InvalidArgumentError("unknown row key sort order");
        }
        if (!IsSupportedRowKeyType(row_key_schema->column_type(i))) {
            return absl::InvalidArgumentError(
                absl::StrCat("unsupported minitable row key type at column ",
                             i,
                             ": ",
                             sstv2::types::data_type_name(row_key_schema->column_type(i))));
        }
    }
    return CellKeyCodec(format, std::move(row_key_schema));
}

uint64_t CellKeyCodec::row_key_schema_fingerprint() const noexcept {
    std::string canonical("minitable-row-key-schema-v1");
    sstv2::codec::append_fixed32(
        &canonical, static_cast<uint32_t>(row_key_schema_->row_key_column_count()));
    for (size_t i = 0; i < row_key_schema_->row_key_column_count(); ++i) {
        sstv2::codec::append_fixed8(
            &canonical, static_cast<uint8_t>(row_key_schema_->column_type(i)));
        sstv2::codec::append_fixed8(
            &canonical, static_cast<uint8_t>(row_key_schema_->column_order(i)));
    }
    return sstv2::codec::crc32c_u64(canonical);
}

absl::Status CellKeyCodec::ValidateRowKey(const std::vector<sstv2::types::Value>& row_key) const {
    if (row_key.size() != row_key_schema_->row_key_column_count()) {
        return absl::InvalidArgumentError("row key column count mismatch");
    }
    for (size_t i = 0; i < row_key.size(); ++i) {
        const auto expected = row_key_schema_->column_type(i);
        if (row_key[i].type() != expected) {
            return absl::InvalidArgumentError(
                absl::StrCat("row key type mismatch at column ",
                             i,
                             ": expected ",
                             sstv2::types::data_type_name(expected),
                             ", got ",
                             sstv2::types::data_type_name(row_key[i].type())));
        }
        if ((expected == DataType::kFloat && std::isnan(row_key[i].as_float())) ||
            (expected == DataType::kDouble && std::isnan(row_key[i].as_double()))) {
            return absl::InvalidArgumentError("NaN is not allowed in a minitable row key");
        }
    }
    return absl::OkStatus();
}

absl::Status CellKeyCodec::CheckEncodedSize(size_t size) const {
    if (size > format_.max_encoded_key_bytes) {
        return absl::ResourceExhaustedError("encoded key exceeds configured limit");
    }
    return absl::OkStatus();
}

absl::StatusOr<std::string> CellKeyCodec::EncodeLogicalRowKey(
    const std::vector<sstv2::types::Value>& row_key) const {
    auto status = ValidateRowKey(row_key);
    if (!status.ok()) {
        return status;
    }

    std::string encoded;
    for (size_t i = 0; i < row_key.size(); ++i) {
        const auto type = row_key_schema_->column_type(i);
        // Minitable canonicalizes signed zero because its logical key equality treats it as zero.
        if (type == DataType::kFloat && row_key[i].as_float() == 0.0F) {
            sstv2::types::Value zero = sstv2::types::Value::make<DataType::kFloat>(0.0F);
            status = sstv2::codec::encode_value_comparable(
                zero, type, row_key_schema_->column_order(i), &encoded);
        } else if (type == DataType::kDouble && row_key[i].as_double() == 0.0) {
            sstv2::types::Value zero = sstv2::types::Value::make<DataType::kDouble>(0.0);
            status = sstv2::codec::encode_value_comparable(
                zero, type, row_key_schema_->column_order(i), &encoded);
        } else {
            status = sstv2::codec::encode_value_comparable(
                row_key[i], type, row_key_schema_->column_order(i), &encoded);
        }
        if (!status.ok()) {
            return status;
        }
        status = CheckEncodedSize(encoded.size());
        if (!status.ok()) {
            return status;
        }
    }
    return encoded;
}

absl::StatusOr<std::vector<sstv2::types::Value>> CellKeyCodec::DecodeLogicalRowKey(
    std::string_view encoded) const {
    if (encoded.size() > format_.max_encoded_key_bytes) {
        return DataLoss("encoded logical row key exceeds format limit");
    }
    DecodeCursor cursor{.input = encoded};
    std::vector<Value> row_key;
    try {
        row_key.reserve(row_key_schema_->row_key_column_count());
        for (size_t i = 0; i < row_key_schema_->row_key_column_count(); ++i) {
            auto value = DecodeRowKeyValue(
                row_key_schema_->column_type(i), row_key_schema_->column_order(i), &cursor);
            if (!value.ok()) {
                return value.status();
            }
            row_key.push_back(std::move(*value));
        }
    } catch (const std::bad_alloc&) {
        return absl::ResourceExhaustedError("logical row key decode allocation failed");
    }
    if (cursor.remaining() != 0) {
        return DataLoss("logical row key has trailing bytes");
    }
    auto canonical = EncodeLogicalRowKey(row_key);
    if (!canonical.ok() || *canonical != encoded) {
        return DataLoss("logical row key is non-canonical");
    }
    return row_key;
}

absl::Status CellKeyCodec::AppendRecordTarget(const RecordTarget& target,
                                              std::string* encoded) const {
    return std::visit(
        [this, encoded](const auto& value) -> absl::Status {
            using Target = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Target, RowTombstone>) {
                sstv2::codec::encode_uint8(kRowTombstonePrefix, encoded);
                sstv2::codec::encode_ordered_uint32(KeyFormat::kReservedColumnFamilyId, encoded);
            } else if constexpr (std::is_same_v<Target, ColumnFamilyTombstone>) {
                if (value.column_family_id == KeyFormat::kReservedColumnFamilyId) {
                    return absl::InvalidArgumentError(
                        "column-family tombstone requires a non-zero column-family ID");
                }
                sstv2::codec::encode_uint8(kColumnFamilyTombstonePrefix, encoded);
                sstv2::codec::encode_ordered_uint32(value.column_family_id, encoded);
            } else {
                if (value.column_family_id == KeyFormat::kReservedColumnFamilyId) {
                    return absl::InvalidArgumentError("cell requires a non-zero column-family ID");
                }
                sstv2::codec::encode_uint8(kCellPrefix, encoded);
                sstv2::codec::encode_ordered_uint32(value.column_family_id, encoded);
                return std::visit(
                    [this, encoded](const auto& qualifier) -> absl::Status {
                        using QualifierType = std::decay_t<decltype(qualifier)>;
                        if constexpr (std::is_same_v<QualifierType, StaticQualifier>) {
                            if (qualifier.column_id == 0) {
                                return absl::InvalidArgumentError(
                                    "static qualifier requires a non-zero column ID");
                            }
                            sstv2::codec::encode_uint8(kStaticQualifierPrefix, encoded);
                            sstv2::codec::encode_ordered_uint32(qualifier.column_id, encoded);
                        } else {
                            if (qualifier.value.size() > format_.max_dynamic_qualifier_bytes) {
                                return absl::ResourceExhaustedError(
                                    "dynamic qualifier exceeds configured limit");
                            }
                            sstv2::codec::encode_uint8(kDynamicQualifierPrefix, encoded);
                            sstv2::codec::encode_bytes(qualifier.value, encoded);
                        }
                        return absl::OkStatus();
                    },
                    value.qualifier);
            }
            return absl::OkStatus();
        },
        target);
}

absl::StatusOr<std::string> CellKeyCodec::EncodeStorageKey(const StorageKey& key) const {
    auto row_key = EncodeLogicalRowKey(key.row_key);
    if (!row_key.ok()) {
        return row_key.status();
    }

    std::string encoded;
    encoded.reserve(row_key->size() + 32);
    if (format_.partition_mode == PartitionMode::kHash) {
        const auto* prefix = std::get_if<HashPrefix>(&key.partition);
        if (prefix == nullptr || prefix->virtual_bucket_id >= format_.virtual_bucket_count) {
            return absl::InvalidArgumentError("HASH key has an invalid virtual bucket");
        }
        sstv2::codec::encode_uint32(prefix->virtual_bucket_id, &encoded);
    } else if (!std::holds_alternative<GlobalOrderPrefix>(key.partition)) {
        return absl::InvalidArgumentError("GLOBAL_ORDER key has a HASH partition prefix");
    }
    encoded.append(*row_key);
    auto status = AppendRecordTarget(key.target, &encoded);
    if (!status.ok()) {
        return status;
    }
    status = CheckEncodedSize(encoded.size());
    if (!status.ok()) {
        return status;
    }
    return encoded;
}

absl::StatusOr<std::string> CellKeyCodec::EncodeVersionedStorageKey(
    const VersionedStorageKey& key) const {
    if (!IsKnownOpType(key.op_type)) {
        return absl::InvalidArgumentError("unknown operation type");
    }
    if (!std::holds_alternative<CellRef>(key.storage_key.target) &&
        key.op_type != OpType::kDelete) {
        return absl::InvalidArgumentError("row and column-family tombstones require Delete");
    }
    auto encoded = EncodeStorageKey(key.storage_key);
    if (!encoded.ok()) {
        return encoded.status();
    }
    sstv2::codec::encode_uint64_desc(key.commit_ts.domain_epoch, &*encoded);
    sstv2::codec::encode_uint64_desc(key.commit_ts.counter, &*encoded);
    sstv2::codec::encode_uint32_desc(key.mutation_seq, &*encoded);
    sstv2::codec::encode_uint8(static_cast<uint8_t>(key.op_type), &*encoded);
    auto status = CheckEncodedSize(encoded->size());
    if (!status.ok()) {
        return status;
    }
    return encoded;
}

absl::StatusOr<StorageKey> CellKeyCodec::DecodeStorageKeyPrefix(std::string_view encoded,
                                                                size_t* consumed) const {
    if (consumed == nullptr || encoded.size() > format_.max_encoded_key_bytes) {
        return DataLoss("invalid encoded storage key size");
    }
    DecodeCursor cursor{.input = encoded};
    StorageKey key;
    if (format_.partition_mode == PartitionMode::kHash) {
        uint32_t virtual_bucket_id = 0;
        if (sstv2::codec::decode_uint32(cursor.data(), cursor.remaining(), &virtual_bucket_id) ==
            0) {
            return DataLoss("truncated virtual bucket ID");
        }
        if (virtual_bucket_id >= format_.virtual_bucket_count) {
            return DataLoss("virtual bucket ID is outside the table format");
        }
        cursor.Consume(KeyFormat::kVirtualBucketWidth);
        key.partition = HashPrefix{.virtual_bucket_id = virtual_bucket_id};
    } else {
        key.partition = GlobalOrderPrefix{};
    }

    key.row_key.reserve(row_key_schema_->row_key_column_count());
    for (size_t i = 0; i < row_key_schema_->row_key_column_count(); ++i) {
        auto value = DecodeRowKeyValue(
            row_key_schema_->column_type(i), row_key_schema_->column_order(i), &cursor);
        if (!value.ok()) {
            return value.status();
        }
        key.row_key.push_back(std::move(*value));
    }

    uint8_t record_prefix = 0;
    if (sstv2::codec::decode_uint8(cursor.data(), cursor.remaining(), &record_prefix) == 0) {
        return DataLoss("missing record prefix");
    }
    cursor.Consume(1);
    uint32_t column_family_id = 0;
    const size_t cf_bytes =
        sstv2::codec::decode_ordered_uint32(cursor.data(), cursor.remaining(), &column_family_id);
    if (cf_bytes == 0 || !cursor.Consume(cf_bytes)) {
        return DataLoss("invalid column-family ID");
    }

    if (record_prefix == kRowTombstonePrefix) {
        if (column_family_id != KeyFormat::kReservedColumnFamilyId) {
            return DataLoss("row tombstone has a non-zero column-family ID");
        }
        key.target = RowTombstone{};
    } else if (record_prefix == kColumnFamilyTombstonePrefix) {
        if (column_family_id == KeyFormat::kReservedColumnFamilyId) {
            return DataLoss("column-family tombstone has a zero column-family ID");
        }
        key.target = ColumnFamilyTombstone{.column_family_id = column_family_id};
    } else if (record_prefix == kCellPrefix) {
        if (column_family_id == KeyFormat::kReservedColumnFamilyId) {
            return DataLoss("cell has a zero column-family ID");
        }
        uint8_t qualifier_prefix = 0;
        if (sstv2::codec::decode_uint8(cursor.data(), cursor.remaining(), &qualifier_prefix) == 0) {
            return DataLoss("missing qualifier prefix");
        }
        cursor.Consume(1);
        Qualifier qualifier;
        if (qualifier_prefix == kStaticQualifierPrefix) {
            uint32_t column_id = 0;
            const size_t column_bytes =
                sstv2::codec::decode_ordered_uint32(cursor.data(), cursor.remaining(), &column_id);
            if (column_bytes == 0 || column_id == 0 || !cursor.Consume(column_bytes)) {
                return DataLoss("invalid static qualifier");
            }
            qualifier = StaticQualifier{.column_id = column_id};
        } else if (qualifier_prefix == kDynamicQualifierPrefix) {
            std::string value;
            const size_t qualifier_bytes =
                sstv2::codec::decode_bytes(cursor.data(), cursor.remaining(), &value);
            if (qualifier_bytes == 0 || !cursor.Consume(qualifier_bytes)) {
                return DataLoss("invalid dynamic qualifier");
            }
            if (value.size() > format_.max_dynamic_qualifier_bytes) {
                return DataLoss("dynamic qualifier exceeds configured limit");
            }
            qualifier = DynamicQualifier{.value = std::move(value)};
        } else {
            return DataLoss("unknown qualifier prefix");
        }
        key.target =
            CellRef{.column_family_id = column_family_id, .qualifier = std::move(qualifier)};
    } else {
        return DataLoss("unknown record prefix");
    }

    *consumed = cursor.offset;
    return key;
}

absl::StatusOr<StorageKey> CellKeyCodec::DecodeStorageKey(std::string_view encoded) const {
    size_t consumed = 0;
    auto key = DecodeStorageKeyPrefix(encoded, &consumed);
    if (!key.ok()) {
        return key.status();
    }
    if (consumed != encoded.size()) {
        return DataLoss("trailing bytes after storage key");
    }
    return key;
}

absl::StatusOr<VersionedStorageKey> CellKeyCodec::DecodeVersionedStorageKey(
    std::string_view encoded) const {
    size_t consumed = 0;
    auto storage_key = DecodeStorageKeyPrefix(encoded, &consumed);
    if (!storage_key.ok()) {
        return storage_key.status();
    }
    DecodeCursor cursor{.input = encoded, .offset = consumed};
    VersionedStorageKey key{.storage_key = std::move(*storage_key)};
    size_t bytes = sstv2::codec::decode_uint64_desc(
        cursor.data(), cursor.remaining(), &key.commit_ts.domain_epoch);
    if (bytes == 0 || !cursor.Consume(bytes))
        return DataLoss("truncated timestamp epoch");
    bytes =
        sstv2::codec::decode_uint64_desc(cursor.data(), cursor.remaining(), &key.commit_ts.counter);
    if (bytes == 0 || !cursor.Consume(bytes))
        return DataLoss("truncated timestamp counter");
    bytes = sstv2::codec::decode_uint32_desc(cursor.data(), cursor.remaining(), &key.mutation_seq);
    if (bytes == 0 || !cursor.Consume(bytes))
        return DataLoss("truncated mutation sequence");
    uint8_t op_type = 0;
    bytes = sstv2::codec::decode_uint8(cursor.data(), cursor.remaining(), &op_type);
    if (bytes == 0 || !cursor.Consume(bytes))
        return DataLoss("missing operation type");
    key.op_type = static_cast<OpType>(op_type);
    if (!IsKnownOpType(key.op_type))
        return DataLoss("unknown operation type");
    if (!std::holds_alternative<CellRef>(key.storage_key.target) &&
        key.op_type != OpType::kDelete) {
        return DataLoss("row or column-family tombstone has a non-Delete operation");
    }
    if (cursor.remaining() != 0)
        return DataLoss("trailing bytes after versioned storage key");
    return key;
}

} // namespace pl::minitable::codec
