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
// Created: 2026/06/05 00:23

#include <gtest/gtest.h>
#include <memory>

#include "cpp/pl/sstv2/types/internal_schema.h"
#include "cpp/pl/sstv2/types/schema.h"

namespace pl::sstv2::types {
namespace {

// =============================================================================
// Schema (user-facing) tests.
// =============================================================================

TEST(SchemaTest, BuilderRejectsEmptySchema) {
    SchemaBuilder builder;
    auto result = builder.build();
    EXPECT_FALSE(result.has_value());
    EXPECT_FALSE(builder.error().empty());
}

TEST(SchemaTest, BuilderSingleColumn) {
    auto result = SchemaBuilder().add_column("user_id", DataType::kUint64).build();
    ASSERT_TRUE(result.has_value());
    auto& schema = *result;
    EXPECT_EQ(schema.row_key_column_count(), 1u);
    EXPECT_EQ(schema.column_name(0), "user_id");
    EXPECT_EQ(schema.column_type(0), DataType::kUint64);
    EXPECT_EQ(schema.column_order(0), SortOrder::kAscending);
}

TEST(SchemaTest, BuilderMultipleColumns) {
    auto result = SchemaBuilder()
                      .add_column("region", DataType::kString)
                      .add_column("timestamp", DataType::kInt64, SortOrder::kDescending)
                      .add_column("seq", DataType::kUint32)
                      .build();
    ASSERT_TRUE(result.has_value());
    auto& schema = *result;
    EXPECT_EQ(schema.row_key_column_count(), 3u);

    EXPECT_EQ(schema.column_name(0), "region");
    EXPECT_EQ(schema.column_type(0), DataType::kString);

    EXPECT_EQ(schema.column_name(1), "timestamp");
    EXPECT_EQ(schema.column_type(1), DataType::kInt64);
    EXPECT_EQ(schema.column_order(1), SortOrder::kDescending);

    EXPECT_EQ(schema.column_name(2), "seq");
    EXPECT_EQ(schema.column_type(2), DataType::kUint32);
}

TEST(SchemaTest, RangeForIteration) {
    auto result =
        SchemaBuilder().add_column("a", DataType::kBool).add_column("b", DataType::kDouble).build();
    ASSERT_TRUE(result.has_value());
    size_t count = 0;
    for (const auto& col : *result) {
        (void)col;
        ++count;
    }
    EXPECT_EQ(count, 2u);
}

TEST(SchemaTest, BuilderRejectsEmptyName) {
    SchemaBuilder builder;
    builder.add_column("", DataType::kInt32);
    auto result = builder.build();
    EXPECT_FALSE(result.has_value());
    EXPECT_FALSE(builder.error().empty());
}

TEST(SchemaTest, BuilderRejectsDuplicateName) {
    SchemaBuilder builder;
    builder.add_column("id", DataType::kUint64);
    builder.add_column("id", DataType::kInt32);
    auto result = builder.build();
    EXPECT_FALSE(result.has_value());
    EXPECT_FALSE(builder.error().empty());
}

TEST(SchemaTest, BuilderAcceptsArrayKey) {
    auto result = SchemaBuilder().add_column("tags", DataType::kArray).build();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->row_key_column_count(), 1u);
    EXPECT_EQ(result->column_type(0), DataType::kArray);
}

TEST(SchemaTest, BuilderAcceptsMapKey) {
    auto result = SchemaBuilder().add_column("attrs", DataType::kMap).build();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->row_key_column_count(), 1u);
    EXPECT_EQ(result->column_type(0), DataType::kMap);
}

TEST(SchemaTest, BuilderRejectsPrivateType) {
    SchemaBuilder builder;
    builder.add_column("bad", DataType::kDataBlock);
    auto result = builder.build();
    EXPECT_FALSE(result.has_value());
    EXPECT_FALSE(builder.error().empty());
}

TEST(SchemaTest, DirectConstruction) {
    Schema s({
        ColumnDef{"x", DataType::kUint64, SortOrder::kAscending},
        ColumnDef{"y", DataType::kString, SortOrder::kDescending},
    });
    EXPECT_EQ(s.row_key_column_count(), 2u);
    EXPECT_EQ(s.column(0).name, "x");
    EXPECT_EQ(s.column(1).order, SortOrder::kDescending);
}

// =============================================================================
// InternalSchema tests.
// =============================================================================

TEST(InternalSchemaTest, ColumnCount) {
    auto schema = std::make_shared<const Schema>(*SchemaBuilder()
                                                      .add_column("k1", DataType::kInt64)
                                                      .add_column("k2", DataType::kString)
                                                      .build());
    InternalSchema is(schema);

    EXPECT_EQ(is.user_column_count(), 2u);
    EXPECT_EQ(is.column_count(), 9u);          // 2 + 7
    EXPECT_EQ(is.sort_key_column_count(), 4u); // 2 + 2 (Version, OpType)
}

TEST(InternalSchemaTest, UserColumnsPassthrough) {
    auto schema = std::make_shared<const Schema>(
        *SchemaBuilder()
             .add_column("region", DataType::kString, SortOrder::kAscending)
             .add_column("ts", DataType::kInt64, SortOrder::kDescending)
             .build());
    InternalSchema is(schema);

    EXPECT_EQ(is.column_name(0), "region");
    EXPECT_EQ(is.column_type(0), DataType::kString);
    EXPECT_EQ(is.column_order(0), SortOrder::kAscending);

    EXPECT_EQ(is.column_name(1), "ts");
    EXPECT_EQ(is.column_type(1), DataType::kInt64);
    EXPECT_EQ(is.column_order(1), SortOrder::kDescending);
}

TEST(InternalSchemaTest, SystemColumns) {
    auto schema =
        std::make_shared<const Schema>(*SchemaBuilder().add_column("k", DataType::kUint64).build());
    InternalSchema is(schema);

    // M=1, system columns at indices 1..7
    EXPECT_EQ(is.version_index(), 1u);
    EXPECT_EQ(is.op_type_index(), 2u);
    EXPECT_EQ(is.flag_index(), 3u);
    EXPECT_EQ(is.filename_index(), 4u);
    EXPECT_EQ(is.offset_index(), 5u);
    EXPECT_EQ(is.length_index(), 6u);
    EXPECT_EQ(is.checksum_index(), 7u);

    // Version is descending, OpType is ascending.
    EXPECT_EQ(is.column_name(1), "Version");
    EXPECT_EQ(is.column_type(1), DataType::kVersion);
    EXPECT_EQ(is.column_order(1), SortOrder::kDescending);

    EXPECT_EQ(is.column_name(2), "OpType");
    EXPECT_EQ(is.column_type(2), DataType::kUint8);
    EXPECT_EQ(is.column_order(2), SortOrder::kAscending);

    // Payload columns.
    EXPECT_EQ(is.column_name(3), "Flag");
    EXPECT_EQ(is.column_type(3), DataType::kUint64);

    EXPECT_EQ(is.column_name(4), "Filename");
    EXPECT_EQ(is.column_type(4), DataType::kString);

    EXPECT_EQ(is.column_name(5), "Offset");
    EXPECT_EQ(is.column_type(5), DataType::kUint64);

    EXPECT_EQ(is.column_name(6), "Length");
    EXPECT_EQ(is.column_type(6), DataType::kUint64);

    EXPECT_EQ(is.column_name(7), "Checksum");
    EXPECT_EQ(is.column_type(7), DataType::kUint64);
}

TEST(InternalSchemaTest, IsSortColumn) {
    auto schema = std::make_shared<const Schema>(*SchemaBuilder()
                                                      .add_column("a", DataType::kUint32)
                                                      .add_column("b", DataType::kString)
                                                      .build());
    InternalSchema is(schema);

    // Sort columns: a(0), b(1), Version(2), OpType(3)
    EXPECT_TRUE(is.is_sort_column(0));
    EXPECT_TRUE(is.is_sort_column(1));
    EXPECT_TRUE(is.is_sort_column(2));
    EXPECT_TRUE(is.is_sort_column(3));
    // Non-sort: Flag(4), Filename(5), Offset(6), Length(7), Checksum(8)
    EXPECT_FALSE(is.is_sort_column(4));
    EXPECT_FALSE(is.is_sort_column(5));
    EXPECT_FALSE(is.is_sort_column(8));
}

} // namespace
} // namespace pl::sstv2::types
