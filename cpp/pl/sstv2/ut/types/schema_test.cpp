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
// Created: 2026/06/04 12:01

#include <vector>

#include "cpp/pl/sstv2/types/schema.h"
#include "gtest/gtest.h"

using namespace pl::sstv2::types;

// === ExternalSchema basics ===

TEST(ExternalSchemaTest, Construction) {
    std::vector<ColumnDef> cols = {
        {.name = "key", .type = DataType::kString},
        {.name = "age", .type = DataType::kInt32},
        {.name = "score", .type = DataType::kDouble},
    };
    ExternalSchema schema(cols);
    EXPECT_EQ(schema.num_columns(), 3u);
}

TEST(ExternalSchemaTest, ColumnLookup) {
    std::vector<ColumnDef> cols = {
        {.name = "id", .type = DataType::kUint64},
        {.name = "name", .type = DataType::kString},
        {.name = "value", .type = DataType::kFloat},
    };
    ExternalSchema schema(cols);

    auto idx = schema.find_column("name");
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 1u);

    EXPECT_FALSE(schema.find_column("nonexistent").has_value());
}

TEST(ExternalSchemaTest, RowKeyColumn) {
    std::vector<ColumnDef> cols = {
        {.name = "primary_key", .type = DataType::kString},
        {.name = "col1", .type = DataType::kInt32},
    };
    ExternalSchema schema(cols);

    const auto& rk = schema.row_key_column();
    EXPECT_EQ(rk.name, "primary_key");
    EXPECT_EQ(rk.type, DataType::kString);
}

// === InternalSchema decomposition ===

TEST(InternalSchemaTest, FixedTypeColumn) {
    // A fixed-type column should produce exactly 1 sub-column
    std::vector<ColumnDef> cols = {
        {.name = "key", .type = DataType::kString},
        {.name = "count", .type = DataType::kInt32},
    };
    ExternalSchema ext(cols);
    auto internal = InternalSchema::from_external(ext);

    auto [start, end] = internal.sub_column_range(1);
    EXPECT_EQ(end - start, 1u);

    auto flag = internal.flag(start);
    EXPECT_EQ(flag.type, DataType::kInt32);
    EXPECT_FALSE(flag.compound_bit);
    EXPECT_FALSE(flag.bitmap_bit);
}

TEST(InternalSchemaTest, StringColumn) {
    // A string column should produce 2 sub-columns: length + data
    std::vector<ColumnDef> cols = {
        {.name = "key", .type = DataType::kUint64},
        {.name = "text", .type = DataType::kString},
    };
    ExternalSchema ext(cols);
    auto internal = InternalSchema::from_external(ext);

    auto [start, end] = internal.sub_column_range(1);
    EXPECT_EQ(end - start, 2u);
}

TEST(InternalSchemaTest, NullableColumn) {
    // A nullable column should get a bitmap flag
    std::vector<ColumnDef> cols = {
        {.name = "key", .type = DataType::kString},
        {.name = "opt_val", .type = DataType::kInt64, .nullable = true},
    };
    ExternalSchema ext(cols);
    auto internal = InternalSchema::from_external(ext);

    auto [start, end] = internal.sub_column_range(1);
    // Check that at least one sub-column has bitmap_bit set
    bool found_bitmap = false;
    for (size_t i = start; i < end; ++i) {
        if (internal.flag(i).bitmap_bit) {
            found_bitmap = true;
            break;
        }
    }
    EXPECT_TRUE(found_bitmap);
}

TEST(InternalSchemaTest, ArrayColumn) {
    // Array column → 3 sub-columns (offsets, lengths, data)
    std::vector<ColumnDef> cols = {
        {.name = "key", .type = DataType::kString},
        {.name = "tags", .type = DataType::kArray, .element_type = DataType::kString},
    };
    ExternalSchema ext(cols);
    auto internal = InternalSchema::from_external(ext);

    auto [start, end] = internal.sub_column_range(1);
    EXPECT_EQ(end - start, 3u);

    // Sub-columns of array should have compound_bit set
    for (size_t i = start; i < end; ++i) {
        EXPECT_TRUE(internal.flag(i).compound_bit);
    }
}

TEST(InternalSchemaTest, MapColumn) {
    // Map column → 5 sub-columns
    std::vector<ColumnDef> cols = {
        {.name = "key", .type = DataType::kString},
        {.name = "metadata",
         .type = DataType::kMap,
         .key_type = DataType::kString,
         .value_type = DataType::kInt32},
    };
    ExternalSchema ext(cols);
    auto internal = InternalSchema::from_external(ext);

    auto [start, end] = internal.sub_column_range(1);
    EXPECT_EQ(end - start, 5u);

    // Sub-columns of map should have compound_bit set
    for (size_t i = start; i < end; ++i) {
        EXPECT_TRUE(internal.flag(i).compound_bit);
    }
}

TEST(InternalSchemaTest, SubColumnRangeMapping) {
    // Multiple columns: verify ranges are contiguous and non-overlapping
    std::vector<ColumnDef> cols = {
        {.name = "key", .type = DataType::kString},  // 2 sub-cols
        {.name = "age", .type = DataType::kInt32},   // 1 sub-col
        {.name = "name", .type = DataType::kString}, // 2 sub-cols
    };
    ExternalSchema ext(cols);
    auto internal = InternalSchema::from_external(ext);

    auto [s0, e0] = internal.sub_column_range(0);
    auto [s1, e1] = internal.sub_column_range(1);
    auto [s2, e2] = internal.sub_column_range(2);

    // Ranges should be contiguous
    EXPECT_EQ(e0, s1);
    EXPECT_EQ(e1, s2);

    // Total sub-columns = 2 + 1 + 2 = 5
    EXPECT_EQ(internal.num_sub_columns(), 5u);
}
