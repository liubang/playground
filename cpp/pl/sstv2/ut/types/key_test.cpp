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
// Created: 2026/06/12 00:00

#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <string>

#include "cpp/pl/sstv2/codec/value_comparable.h"
#include "cpp/pl/sstv2/types/key.h"
#include "cpp/pl/sstv2/types/op_type.h"
#include "cpp/pl/sstv2/types/schema.h"

namespace pl::sstv2::types {
namespace {

Schema::ConstRef make_schema() {
    auto schema = SchemaBuilder()
                      .add_column("tenant", DataType::kString)
                      .add_column("score", DataType::kUint64, SortOrder::kDescending)
                      .build();
    EXPECT_TRUE(schema.has_value());
    return std::make_shared<const Schema>(std::move(*schema));
}

InternalRow make_row(InternalSchema::ConstRef schema,
                     std::string tenant,
                     uint64_t score,
                     Version version = Version{.major = 10}) {
    InternalRow row = InternalRow::make(schema);
    row.columns[0] = Value::make<DataType::kString>(std::move(tenant));
    row.columns[1] = Value::make<DataType::kUint64>(score);
    row.columns[schema->version_index()] = Value::make<DataType::kVersion>(version);
    row.columns[schema->op_type_index()] =
        Value::make<DataType::kUint8>(static_cast<uint8_t>(OpType::kPut));
    row.columns[schema->flag_index()] = Value::make<DataType::kUint64>(uint64_t{0});
    row.columns[schema->filename_index()] = Value::make<DataType::kString>(kEmbeddedFilename);
    row.columns[schema->offset_index()] = Value::make<DataType::kUint64>(uint64_t{0});
    row.columns[schema->length_index()] = Value::make<DataType::kUint64>(uint64_t{0});
    row.columns[schema->checksum_index()] = Value::make<DataType::kUint64>(uint64_t{0});
    return row;
}

TEST(KeyTest, ComparesAllKeyBySchemaColumnsAndSortOrder) {
    auto schema = InternalSchema::make(make_schema());
    auto high = make_all_key(make_row(schema, "a", 20), schema);
    auto low = make_all_key(make_row(schema, "a", 10), schema);
    ASSERT_TRUE(high.ok()) << high.status();
    ASSERT_TRUE(low.ok()) << low.status();

    KeyComparator comparator(schema);
    auto cmp = comparator.compare_all_key(*high, *low);
    ASSERT_TRUE(cmp.ok()) << cmp.status();
    EXPECT_LT(*cmp, 0);
}

TEST(KeyTest, LogicalCompareMatchesMemComparableEncoding) {
    auto schema = InternalSchema::make(make_schema());
    const auto lhs_row = make_row(schema, "a", 20);
    const auto rhs_row = make_row(schema, "a", 10);
    auto lhs = make_all_key(lhs_row, schema);
    auto rhs = make_all_key(rhs_row, schema);
    ASSERT_TRUE(lhs.ok()) << lhs.status();
    ASSERT_TRUE(rhs.ok()) << rhs.status();

    KeyComparator comparator(schema);
    auto logical = comparator.compare_all_key(*lhs, *rhs);
    ASSERT_TRUE(logical.ok()) << logical.status();

    auto encoded_lhs = codec::make_encoded_all_key(lhs_row, schema);
    auto encoded_rhs = codec::make_encoded_all_key(rhs_row, schema);
    ASSERT_TRUE(encoded_lhs.ok()) << encoded_lhs.status();
    ASSERT_TRUE(encoded_rhs.ok()) << encoded_rhs.status();
    EXPECT_LT(*logical, 0);
    EXPECT_LT(compare_encoded_bytes(*encoded_lhs, *encoded_rhs), 0);
}

TEST(KeyTest, PrefixBoundaryMatchesAllKeysWithSamePrefix) {
    auto schema = InternalSchema::make(make_schema());
    auto prefix = make_prefix_key(KeyPrefix{.key_columns = {Value::make<DataType::kString>("b")}},
                                  schema->user_schema(),
                                  schema);
    auto before = make_all_key(make_row(schema, "a", 1), schema);
    auto inside = make_all_key(make_row(schema, "b", 100), schema);
    ASSERT_TRUE(prefix.ok()) << prefix.status();
    ASSERT_TRUE(before.ok()) << before.status();
    ASSERT_TRUE(inside.ok()) << inside.status();

    KeyComparator comparator(schema);
    auto before_cmp = comparator.compare_all_key_to_prefix(*before, *prefix);
    auto inside_cmp = comparator.compare_all_key_to_prefix(*inside, *prefix);
    ASSERT_TRUE(before_cmp.ok()) << before_cmp.status();
    ASSERT_TRUE(inside_cmp.ok()) << inside_cmp.status();
    EXPECT_LT(*before_cmp, 0);
    EXPECT_EQ(*inside_cmp, 0);
}

} // namespace
} // namespace pl::sstv2::types
