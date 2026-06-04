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

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/pl/sstv2/block/block_reader.h"
#include "cpp/pl/sstv2/block/block_writer.h"
#include "cpp/pl/sstv2/block/column_store.h"
#include "gtest/gtest.h"

using namespace pl::sstv2::block;

namespace {

// Helper to view a std::string's bytes as a span<const std::byte>.
std::span<const std::byte> as_bytes(const std::string& s) {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size());
}

} // namespace

// === ColumnStoreBuilder + ColumnStoreReader round-trip ===
//
// This exercises the column store in isolation: build a 3 sub-column store
// with a mix of values and nulls, then read it back and assert get()/is_null()
// behave correctly.
TEST(ColumnStoreTest, RoundTrip) {
    constexpr size_t kNumSubColumns = 3;
    ColumnStoreBuilder builder(kNumSubColumns);

    // Row 0: all present.
    builder.add_value(0, 10);
    builder.add_value(1, 20);
    builder.add_value(2, 30);
    builder.finish_row();

    // Row 1: sub-column 1 is null.
    builder.add_value(0, 11);
    builder.add_null(1);
    builder.add_value(2, 31);
    builder.finish_row();

    // Row 2: sub-column 0 and 2 null.
    builder.add_null(0);
    builder.add_value(1, 22);
    builder.add_null(2);
    builder.finish_row();

    // Row 3: all present.
    builder.add_value(0, 13);
    builder.add_value(1, 23);
    builder.add_value(2, 33);
    builder.finish_row();

    constexpr size_t kNumRows = 4;

    auto built = builder.build();
    ASSERT_TRUE(built.ok()) << built.status();
    const std::string& data = built.value();

    // Metadata is valid only after build().
    const std::vector<SubColumnMeta>& metas = builder.sub_column_metas();
    ASSERT_EQ(metas.size(), kNumSubColumns);

    ColumnStoreReader reader(as_bytes(data), metas, kNumRows);

    // Row 0.
    EXPECT_FALSE(reader.is_null(0, 0));
    EXPECT_FALSE(reader.is_null(1, 0));
    EXPECT_FALSE(reader.is_null(2, 0));
    EXPECT_EQ(reader.get(0, 0), 10u);
    EXPECT_EQ(reader.get(1, 0), 20u);
    EXPECT_EQ(reader.get(2, 0), 30u);

    // Row 1: sub-column 1 null.
    EXPECT_FALSE(reader.is_null(0, 1));
    EXPECT_TRUE(reader.is_null(1, 1));
    EXPECT_FALSE(reader.is_null(2, 1));
    EXPECT_EQ(reader.get(0, 1), 11u);
    EXPECT_EQ(reader.get(2, 1), 31u);

    // Row 2: sub-columns 0 and 2 null.
    EXPECT_TRUE(reader.is_null(0, 2));
    EXPECT_FALSE(reader.is_null(1, 2));
    EXPECT_TRUE(reader.is_null(2, 2));
    EXPECT_EQ(reader.get(1, 2), 22u);

    // Row 3.
    EXPECT_FALSE(reader.is_null(0, 3));
    EXPECT_FALSE(reader.is_null(1, 3));
    EXPECT_FALSE(reader.is_null(2, 3));
    EXPECT_EQ(reader.get(0, 3), 13u);
    EXPECT_EQ(reader.get(1, 3), 23u);
    EXPECT_EQ(reader.get(2, 3), 33u);
}

// === BlockWriter basics: empty block ===
//
// A freshly constructed writer reports empty()/num_rows() == 0.
// finish() on an empty block returns an error (no rows to write).
TEST(BlockWriterReaderTest, Empty) {
    BlockWriter writer(/*num_sub_columns=*/2, /*num_var_sub_columns=*/1);

    EXPECT_TRUE(writer.empty());
    EXPECT_EQ(writer.num_rows(), 0u);

    auto finished = writer.finish();
    EXPECT_FALSE(finished.ok());
}

// === BlockWriter basics: num_rows + first/last row key tracking ===
TEST(BlockWriterReaderTest, FirstLastKey) {
    BlockWriter writer(/*num_sub_columns=*/2, /*num_var_sub_columns=*/1);

    const std::vector<std::string_view> keys = {"key_001", "key_002", "key_003"};
    for (const auto& key : keys) {
        const uint64_t fixed[] = {1, 2};
        const std::string_view vars[] = {"v"};
        ASSERT_TRUE(writer.add_row(key, fixed, vars));
    }

    EXPECT_FALSE(writer.empty());
    EXPECT_EQ(writer.num_rows(), keys.size());
    EXPECT_EQ(writer.first_row_key(), "key_001");
    EXPECT_EQ(writer.last_row_key(), "key_003");
}

// === BlockWriter -> BlockReader full round-trip ===
//
// NOTE: BlockReader::open in the current header is
//   open(block_data, num_sub_columns, num_var_sub_columns)
// and the accessors take arguments as (row_idx, sub_col_idx) /
// (row_idx, var_col_idx), which is the reverse of some draft docs. The asserts
// below follow the actual block_reader.h signatures. If the reader interface
// changes, the open() call and accessor argument order may need adjustment.
TEST(BlockWriterReaderTest, RoundTrip) {
    constexpr size_t kNumSubColumns = 2;
    constexpr size_t kNumVarSubColumns = 1;

    BlockWriter writer(kNumSubColumns, kNumVarSubColumns);

    struct Row {
        std::string_view key;
        uint64_t fixed[kNumSubColumns];
        std::string_view var;
    };

    const Row rows[] = {
        {"key_001", {100, 200}, "hello"},
        {"key_002", {101, 201}, "world"},
        {"key_003", {102, 202}, "foo"},
        {"key_004", {103, 203}, "bar"},
        {"key_005", {104, 204}, "baz"},
    };
    constexpr size_t kNumRows = 5;

    for (const auto& row : rows) {
        const std::string_view vars[] = {row.var};
        ASSERT_TRUE(writer.add_row(row.key, row.fixed, vars)) << "failed to add " << row.key;
    }

    EXPECT_EQ(writer.num_rows(), kNumRows);

    auto finished = writer.finish();
    ASSERT_TRUE(finished.ok()) << finished.status();
    const std::string& block_data = finished.value();

    auto reader_or = BlockReader::open(as_bytes(block_data), kNumSubColumns, kNumVarSubColumns);
    ASSERT_TRUE(reader_or.ok()) << reader_or.status();
    const BlockReader& reader = reader_or.value();

    ASSERT_EQ(reader.num_rows(), kNumRows);

    for (size_t i = 0; i < kNumRows; ++i) {
        EXPECT_EQ(reader.row_key(i), rows[i].key) << "row " << i;
        EXPECT_EQ(reader.get_fixed(i, 0), rows[i].fixed[0]) << "row " << i << " col 0";
        EXPECT_EQ(reader.get_fixed(i, 1), rows[i].fixed[1]) << "row " << i << " col 1";
        EXPECT_EQ(reader.get_var(i, 0), rows[i].var) << "row " << i << " var 0";
    }
}
