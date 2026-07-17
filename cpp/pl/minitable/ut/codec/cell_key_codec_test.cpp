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

#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <string>
#include <vector>

#include "cpp/pl/minitable/codec/cell_key_codec.h"
#include "cpp/pl/sstv2/types/schema.h"
#include "cpp/pl/sstv2/types/value.h"

namespace pl::minitable::codec {
namespace {

using sstv2::types::DataType;
using sstv2::types::OpType;
using sstv2::types::Schema;
using sstv2::types::SchemaBuilder;
using sstv2::types::SortOrder;
using sstv2::types::Value;

Schema::ConstRef MakeSchema() {
    auto schema = SchemaBuilder()
                      .add_column("tenant", DataType::kUint64)
                      .add_column("name", DataType::kString)
                      .build();
    EXPECT_TRUE(schema.has_value());
    return std::make_shared<const Schema>(std::move(*schema));
}

std::vector<Value> MakeRowKey(uint64_t tenant, std::string name) {
    return {Value::make<DataType::kUint64>(tenant),
            Value::make<DataType::kString>(std::move(name))};
}

std::string Hex(std::string_view bytes) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) {
        result.push_back(kDigits[byte >> 4]);
        result.push_back(kDigits[byte & 0x0f]);
    }
    return result;
}

TEST(CellKeyCodecTest, GlobalStaticCellHasStableGoldenEncoding) {
    auto codec =
        CellKeyCodec::Create({.partition_mode = PartitionMode::kGlobalOrder}, MakeSchema());
    ASSERT_TRUE(codec.ok()) << codec.status();

    StorageKey key{
        .row_key = MakeRowKey(7, "ab"),
        .target = CellRef{.column_family_id = 3, .qualifier = StaticQualifier{.column_id = 9}}};
    auto encoded = codec->EncodeStorageKey(key);
    ASSERT_TRUE(encoded.ok()) << encoded.status();
    EXPECT_EQ(Hex(*encoded), "0000000000000007616200000000000002020103000109");
}

TEST(CellKeyCodecTest, HashPrefixAndDynamicQualifierAreCanonical) {
    auto codec = CellKeyCodec::Create({.partition_mode = PartitionMode::kHash,
                                       .hash_algorithm = HashAlgorithm::kXxh3_64V1,
                                       .virtual_bucket_count = 1024},
                                      MakeSchema());
    ASSERT_TRUE(codec.ok()) << codec.status();

    StorageKey key{.partition = HashPrefix{.virtual_bucket_id = 17},
                   .row_key = MakeRowKey(1, ""),
                   .target =
                       CellRef{.column_family_id = 2,
                               .qualifier = DynamicQualifier{.value = std::string("a\0b", 3)}}};
    auto encoded = codec->EncodeStorageKey(key);
    ASSERT_TRUE(encoded.ok()) << encoded.status();
    EXPECT_EQ(Hex(*encoded),
              "00000011000000000000000100000000000000000002010201610062000000000003");
}

TEST(CellKeyCodecTest, VersionSortsNewestFirst) {
    auto codec =
        CellKeyCodec::Create({.partition_mode = PartitionMode::kGlobalOrder}, MakeSchema());
    ASSERT_TRUE(codec.ok()) << codec.status();

    StorageKey storage{
        .row_key = MakeRowKey(1, "row"),
        .target = CellRef{.column_family_id = 1, .qualifier = StaticQualifier{.column_id = 1}}};
    VersionedStorageKey older{.storage_key = storage,
                              .commit_ts = {.domain_epoch = 1, .counter = 10},
                              .mutation_seq = 0,
                              .op_type = OpType::kPut};
    VersionedStorageKey newer = older;
    newer.commit_ts.counter = 11;
    auto older_encoded = codec->EncodeVersionedStorageKey(older);
    auto newer_encoded = codec->EncodeVersionedStorageKey(newer);
    ASSERT_TRUE(older_encoded.ok());
    ASSERT_TRUE(newer_encoded.ok());
    EXPECT_LT(*newer_encoded, *older_encoded);

    newer.commit_ts.counter = 10;
    newer.mutation_seq = 1;
    newer_encoded = codec->EncodeVersionedStorageKey(newer);
    ASSERT_TRUE(newer_encoded.ok());
    EXPECT_LT(*newer_encoded, *older_encoded);
}

TEST(CellKeyCodecTest, RecordTargetsAreUnambiguous) {
    auto codec =
        CellKeyCodec::Create({.partition_mode = PartitionMode::kGlobalOrder}, MakeSchema());
    ASSERT_TRUE(codec.ok());

    StorageKey row_delete{.row_key = MakeRowKey(1, "x"), .target = RowTombstone{}};
    StorageKey family_delete{.row_key = MakeRowKey(1, "x"),
                             .target = ColumnFamilyTombstone{.column_family_id = 1}};
    StorageKey cell{.row_key = MakeRowKey(1, "x"),
                    .target =
                        CellRef{.column_family_id = 1, .qualifier = DynamicQualifier{.value = ""}}};
    auto row = codec->EncodeStorageKey(row_delete);
    auto family = codec->EncodeStorageKey(family_delete);
    auto ordinary = codec->EncodeStorageKey(cell);
    ASSERT_TRUE(row.ok());
    ASSERT_TRUE(family.ok());
    ASSERT_TRUE(ordinary.ok());
    EXPECT_LT(*row, *family);
    EXPECT_LT(*family, *ordinary);
}

TEST(CellKeyCodecTest, RejectsNaNAndCanonicalizesSignedZero) {
    auto schema = SchemaBuilder().add_column("score", DataType::kDouble).build();
    ASSERT_TRUE(schema.has_value());
    auto codec = CellKeyCodec::Create({.partition_mode = PartitionMode::kGlobalOrder},
                                      std::make_shared<const Schema>(std::move(*schema)));
    ASSERT_TRUE(codec.ok());

    std::vector<Value> nan_key{
        Value::make<DataType::kDouble>(std::numeric_limits<double>::quiet_NaN())};
    EXPECT_EQ(codec->EncodeLogicalRowKey(nan_key).status().code(),
              absl::StatusCode::kInvalidArgument);

    auto negative = codec->EncodeLogicalRowKey({Value::make<DataType::kDouble>(-0.0)});
    auto positive = codec->EncodeLogicalRowKey({Value::make<DataType::kDouble>(0.0)});
    ASSERT_TRUE(negative.ok());
    ASSERT_TRUE(positive.ok());
    EXPECT_EQ(*negative, *positive);
}

TEST(CellKeyCodecTest, RejectsInvalidShapesAndEnforcesLimits) {
    auto codec = CellKeyCodec::Create({.partition_mode = PartitionMode::kGlobalOrder,
                                       .max_encoded_key_bytes = 256,
                                       .max_dynamic_qualifier_bytes = 2,
                                       .max_row_key_columns = 2},
                                      MakeSchema());
    ASSERT_TRUE(codec.ok());

    StorageKey bad_cf{
        .row_key = MakeRowKey(1, "x"),
        .target = CellRef{.column_family_id = 0, .qualifier = StaticQualifier{.column_id = 1}}};
    EXPECT_EQ(codec->EncodeStorageKey(bad_cf).status().code(), absl::StatusCode::kInvalidArgument);

    StorageKey bad_bucket{.partition = HashPrefix{.virtual_bucket_id = 1},
                          .row_key = MakeRowKey(1, "x"),
                          .target = RowTombstone{}};
    EXPECT_EQ(codec->EncodeStorageKey(bad_bucket).status().code(),
              absl::StatusCode::kInvalidArgument);

    StorageKey long_qualifier{
        .row_key = MakeRowKey(1, "x"),
        .target = CellRef{.column_family_id = 1, .qualifier = DynamicQualifier{.value = "abc"}}};
    EXPECT_EQ(codec->EncodeStorageKey(long_qualifier).status().code(),
              absl::StatusCode::kResourceExhausted);
}

TEST(CellKeyCodecTest, StrictDecodeRoundTripsAndRejectsCorruption) {
    auto codec = CellKeyCodec::Create({.partition_mode = PartitionMode::kHash,
                                       .hash_algorithm = HashAlgorithm::kXxh3_64V1,
                                       .virtual_bucket_count = 1024},
                                      MakeSchema());
    ASSERT_TRUE(codec.ok());

    VersionedStorageKey original{
        .storage_key = {.partition = HashPrefix{.virtual_bucket_id = 9},
                        .row_key = MakeRowKey(42, "row"),
                        .target = CellRef{.column_family_id = 7,
                                          .qualifier = DynamicQualifier{.value = "q"}}},
        .commit_ts = {.domain_epoch = 3, .counter = 99},
        .mutation_seq = 4,
        .op_type = OpType::kDelete,
    };
    auto encoded = codec->EncodeVersionedStorageKey(original);
    ASSERT_TRUE(encoded.ok());
    auto decoded = codec->DecodeVersionedStorageKey(*encoded);
    ASSERT_TRUE(decoded.ok()) << decoded.status();
    const auto* partition = std::get_if<HashPrefix>(&decoded->storage_key.partition);
    ASSERT_NE(partition, nullptr);
    EXPECT_EQ(partition->virtual_bucket_id, 9U);
    EXPECT_EQ(decoded->storage_key.row_key[0].as_uint64(), 42U);
    EXPECT_EQ(decoded->storage_key.row_key[1].as_string(), "row");
    const auto* cell = std::get_if<CellRef>(&decoded->storage_key.target);
    ASSERT_NE(cell, nullptr);
    EXPECT_EQ(cell->column_family_id, 7U);
    const auto* qualifier = std::get_if<DynamicQualifier>(&cell->qualifier);
    ASSERT_NE(qualifier, nullptr);
    EXPECT_EQ(qualifier->value, "q");
    EXPECT_EQ(decoded->commit_ts, (Timestamp{.domain_epoch = 3, .counter = 99}));
    EXPECT_EQ(decoded->mutation_seq, 4U);
    EXPECT_EQ(decoded->op_type, OpType::kDelete);

    for (size_t length = 0; length < encoded->size(); ++length) {
        EXPECT_EQ(codec->DecodeVersionedStorageKey(std::string_view(encoded->data(), length))
                      .status()
                      .code(),
                  absl::StatusCode::kDataLoss)
            << "accepted truncation at " << length;
    }
    EXPECT_EQ(codec->DecodeVersionedStorageKey(*encoded + "x").status().code(),
              absl::StatusCode::kDataLoss);
}

TEST(CellKeyCodecTest, RejectsNonCanonicalOrderedId) {
    auto codec =
        CellKeyCodec::Create({.partition_mode = PartitionMode::kGlobalOrder}, MakeSchema());
    ASSERT_TRUE(codec.ok());
    StorageKey key{.row_key = MakeRowKey(1, "x"), .target = RowTombstone{}};
    auto encoded = codec->EncodeStorageKey(key);
    ASSERT_TRUE(encoded.ok());
    ASSERT_GE(encoded->size(), 2U);
    // Row tombstone ends in record-prefix 0x00 + canonical CF ID 0x00.
    encoded->back() = '\x01';
    encoded->push_back('\0');
    EXPECT_EQ(codec->DecodeStorageKey(*encoded).status().code(), absl::StatusCode::kDataLoss);
}

TEST(CellKeyCodecTest, RowKeyEncodingPreservesMixedColumnOrder) {
    auto schema = SchemaBuilder()
                      .add_column("a", DataType::kString, SortOrder::kAscending)
                      .add_column("b", DataType::kInt64, SortOrder::kDescending)
                      .build();
    ASSERT_TRUE(schema.has_value());
    auto codec = CellKeyCodec::Create({.partition_mode = PartitionMode::kGlobalOrder},
                                      std::make_shared<const Schema>(std::move(*schema)));
    ASSERT_TRUE(codec.ok());

    auto a = codec->EncodeLogicalRowKey(
        {Value::make<DataType::kString>("same"), Value::make<DataType::kInt64>(2)});
    auto b = codec->EncodeLogicalRowKey(
        {Value::make<DataType::kString>("same"), Value::make<DataType::kInt64>(1)});
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    EXPECT_LT(*a, *b);
}

} // namespace
} // namespace pl::minitable::codec
