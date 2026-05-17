// Copyright (c) 2024 The Authors. All rights reserved.
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
// Created: 2024/07/01 01:02

#include "cpp/pl/arena/arena.h"
#include "cpp/pl/random/random.h"
#include "cpp/pl/sst/cell.h"
#include <gtest/gtest.h>

namespace pl {
class CellTest : public ::testing::Test {
    void SetUp() override {}
    void TearDown() override {}
};

// ==================== Encode/Decode Roundtrip ====================

TEST_F(CellTest, cell_encode_decode_roundtrip) {
    for (int i = 0; i < 10; ++i) {
        std::string rowkey = random_string(32);
        std::string cf = random_string(8);
        std::string col = random_string(16);
        std::string val = random_string(16 * 1024);
        uint64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
        auto ct = (CellType)(i % 4);
        Cell cell(ct, rowkey, cf, col, val, ts);
        std::string encoded_cell_key = cell.cellKey().encode();

        CellKey other_ck;
        auto result = other_ck.decode(encoded_cell_key, rowkey.size());
        ASSERT_TRUE(result.hasValue());
        EXPECT_EQ(ct, other_ck.cell_type);
        EXPECT_EQ(rowkey, other_ck.rowkey);
        EXPECT_EQ(cf, other_ck.cf);
        EXPECT_EQ(col, other_ck.col);
        EXPECT_EQ(ts, other_ck.timestamp);
    }
}

// ==================== CellKey Decode Error Paths ====================

TEST_F(CellTest, decode_too_short) {
    // Minimum encoded size: rowkey_len + 2 (null terminators) + 8 (timestamp) + 1 (type) = rowkey_len + 11
    // With rowkey_len = 5, minimum = 16 bytes. Provide fewer.
    std::string short_data = "hello"; // 5 bytes, rowkey_len = 5, total needed >= 16
    CellKey ck;
    auto result = ck.decode(short_data, 5);
    EXPECT_TRUE(result.hasError());
    EXPECT_EQ(result.error().code(), StatusCode::kInvalidCell);
}

TEST_F(CellTest, decode_empty_input) {
    CellKey ck;
    auto result = ck.decode("", 0);
    EXPECT_TRUE(result.hasError());
    EXPECT_EQ(result.error().code(), StatusCode::kInvalidCell);
}

TEST_F(CellTest, decode_missing_cf_null_terminator) {
    // Build data: rowkey + cf (no null terminator) + rest
    // rowkey_len = 3, data = "abc" + "def" (no '\0')
    std::string data = "abcdef";
    // Length check passes (3 + 11 = 14 > 6) so it returns short error from minimum check
    // Actually minimum check: input.size() < rowkey_len + sizeof(uint64_t) + sizeof(uint8_t) + 2
    //   = 3 + 8 + 1 + 2 = 14. "abcdef" is 6 bytes < 14, so fails at minimum check.
    CellKey ck;
    auto result = ck.decode(data, 3);
    EXPECT_TRUE(result.hasError());
    EXPECT_EQ(result.error().code(), StatusCode::kInvalidCell);
}

TEST_F(CellTest, decode_missing_cf_null_in_valid_length_data) {
    // Build data that passes minimum length check but has no null terminator for cf
    // rowkey_len = 3, need at least 3 + 8 + 1 + 2 = 14 bytes
    // Fill with non-null bytes to trigger "cf_end == nullptr"
    std::string data(14, 'x'); // No '\0' after rowkey position
    CellKey ck;
    auto result = ck.decode(data, 3);
    EXPECT_TRUE(result.hasError());
    EXPECT_EQ(result.error().code(), StatusCode::kInvalidCell);
    // After error, CellKey should be reset
    EXPECT_EQ(ck.rowkey, std::string_view());
    EXPECT_EQ(ck.cf, std::string_view());
    EXPECT_EQ(ck.col, std::string_view());
    EXPECT_EQ(ck.cell_type, CellType::CT_NONE);
}

TEST_F(CellTest, decode_missing_col_null_terminator) {
    // Build data with cf null but no col null
    // rowkey = "abc", cf = "d" + '\0', then fill rest with no '\0'
    std::string data;
    data.append("abc");     // rowkey (3 bytes)
    data.append("d");       // cf
    data.push_back('\0');   // cf null terminator
    data.append(20, 'x');   // col (no null terminator) + padding
    CellKey ck;
    auto result = ck.decode(data, 3);
    EXPECT_TRUE(result.hasError());
    EXPECT_EQ(result.error().code(), StatusCode::kInvalidCell);
}

TEST_F(CellTest, decode_insufficient_data_after_col) {
    // Build data with cf and col null terminators but not enough bytes for timestamp+type
    std::string data;
    data.append("abc");     // rowkey (3 bytes)
    data.append("d");       // cf
    data.push_back('\0');   // cf null terminator
    data.append("e");       // col
    data.push_back('\0');   // col null terminator
    // Now we need 8 (timestamp) + 1 (type) = 9 more bytes, but provide fewer
    data.append(5, '\x01'); // only 5 bytes
    CellKey ck;
    auto result = ck.decode(data, 3);
    EXPECT_TRUE(result.hasError());
    EXPECT_EQ(result.error().code(), StatusCode::kInvalidCell);
}

TEST_F(CellTest, decode_empty_cf_and_col) {
    // Valid decode with empty cf and col (just null terminators)
    std::string rowkey = "myrow";
    std::string cf;
    std::string col;
    uint64_t ts = 12345;
    CellType ct = CellType::CT_PUT;

    CellKey original(rowkey, cf, col, ts, ct);
    std::string encoded = original.encode();

    CellKey decoded;
    auto result = decoded.decode(encoded, rowkey.size());
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(decoded.rowkey, "myrow");
    EXPECT_EQ(decoded.cf, "");
    EXPECT_EQ(decoded.col, "");
    EXPECT_EQ(decoded.timestamp, 12345u);
    EXPECT_EQ(decoded.cell_type, CellType::CT_PUT);
}

TEST_F(CellTest, decode_rowkey_len_larger_than_data) {
    // rowkey_len = 100 but data is only 20 bytes
    std::string data(20, 'a');
    CellKey ck;
    auto result = ck.decode(data, 100);
    EXPECT_TRUE(result.hasError());
    EXPECT_EQ(result.error().code(), StatusCode::kInvalidCell);
}

// ==================== CellKey Comparison ====================

TEST_F(CellTest, cellkey_compare_by_rowkey) {
    auto cmp = std::make_shared<BytewiseComparator>();
    CellKey k1("apple", "cf", "col", 100, CellType::CT_PUT);
    CellKey k2("banana", "cf", "col", 100, CellType::CT_PUT);
    EXPECT_LT(k1.compare(cmp, k2), 0);
    EXPECT_GT(k2.compare(cmp, k1), 0);
}

TEST_F(CellTest, cellkey_compare_by_cf) {
    auto cmp = std::make_shared<BytewiseComparator>();
    CellKey k1("row", "alpha", "col", 100, CellType::CT_PUT);
    CellKey k2("row", "beta", "col", 100, CellType::CT_PUT);
    EXPECT_LT(k1.compare(cmp, k2), 0);
}

TEST_F(CellTest, cellkey_compare_by_col) {
    auto cmp = std::make_shared<BytewiseComparator>();
    CellKey k1("row", "cf", "aaa", 100, CellType::CT_PUT);
    CellKey k2("row", "cf", "bbb", 100, CellType::CT_PUT);
    EXPECT_LT(k1.compare(cmp, k2), 0);
}

TEST_F(CellTest, cellkey_compare_by_timestamp_descending) {
    auto cmp = std::make_shared<BytewiseComparator>();
    CellKey k1("row", "cf", "col", 200, CellType::CT_PUT);
    CellKey k2("row", "cf", "col", 100, CellType::CT_PUT);
    // Larger timestamp should sort first (return negative)
    EXPECT_LT(k1.compare(cmp, k2), 0);
    EXPECT_GT(k2.compare(cmp, k1), 0);
}

TEST_F(CellTest, cellkey_compare_by_type) {
    auto cmp = std::make_shared<BytewiseComparator>();
    CellKey k1("row", "cf", "col", 100, CellType::CT_DEL);
    CellKey k2("row", "cf", "col", 100, CellType::CT_PUT);
    // CT_DEL(0) < CT_PUT(1) so compare returns -1
    EXPECT_LT(k1.compare(cmp, k2), 0);
}

TEST_F(CellTest, cellkey_equality) {
    CellKey k1("row", "cf", "col", 100, CellType::CT_PUT);
    CellKey k2("row", "cf", "col", 100, CellType::CT_PUT);
    CellKey k3("row", "cf", "col", 200, CellType::CT_PUT);
    EXPECT_EQ(k1, k2);
    EXPECT_NE(k1, k3);
}

// ==================== Cell::clone ====================

TEST_F(CellTest, clone_basic) {
    Arena arena(4096);
    std::string rowkey = "test_row";
    std::string cf = "family";
    std::string col = "column";
    std::string val = "value_data";
    Cell cell(CellType::CT_PUT, rowkey, cf, col, val, 99999);

    auto cloned = cell.clone(&arena);
    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->rowkey(), "test_row");
    EXPECT_EQ(cloned->cf(), "family");
    EXPECT_EQ(cloned->col(), "column");
    EXPECT_EQ(cloned->value(), "value_data");
    EXPECT_EQ(cloned->timestamp(), 99999u);
    EXPECT_EQ(cloned->cell_type(), CellType::CT_PUT);

    // Verify data is actually copied (different memory)
    EXPECT_NE(cloned->rowkey().data(), rowkey.data());
    EXPECT_NE(cloned->value().data(), val.data());
}

TEST_F(CellTest, clone_with_null_arena) {
    std::string rowkey = "test_row";
    std::string cf = "family";
    std::string col = "column";
    std::string val = "value_data";
    Cell cell(CellType::CT_PUT, rowkey, cf, col, val, 100);

    auto cloned = cell.clone(nullptr);
    EXPECT_EQ(cloned, nullptr);
}

TEST_F(CellTest, clone_empty_cell) {
    Arena arena(4096);
    Cell cell;
    auto cloned = cell.clone(&arena);
    ASSERT_NE(cloned, nullptr);
    EXPECT_TRUE(cloned->empty());
    EXPECT_EQ(cloned->total_size(), 0u);
}

TEST_F(CellTest, clone_with_empty_value) {
    Arena arena(4096);
    std::string rowkey = "r";
    std::string cf = "c";
    std::string col = "q";
    Cell cell(CellType::CT_DEL, rowkey, cf, col, "", 500);

    auto cloned = cell.clone(&arena);
    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->rowkey(), "r");
    EXPECT_EQ(cloned->cf(), "c");
    EXPECT_EQ(cloned->col(), "q");
    EXPECT_EQ(cloned->value(), "");
    EXPECT_EQ(cloned->cell_type(), CellType::CT_DEL);
}

// ==================== Cell::empty and total_size ====================

TEST_F(CellTest, empty_default_cell) {
    Cell cell;
    EXPECT_TRUE(cell.empty());
    EXPECT_EQ(cell.total_size(), 0u);
}

TEST_F(CellTest, non_empty_cell) {
    std::string rowkey = "row";
    std::string cf = "cf";
    std::string col = "col";
    std::string val = "value";
    Cell cell(CellType::CT_PUT, rowkey, cf, col, val, 100);
    EXPECT_FALSE(cell.empty());
    // total_size = 3 + 2 + 3 + 5 = 13
    EXPECT_EQ(cell.total_size(), 13u);
}

TEST_F(CellTest, total_size_with_empty_components) {
    std::string rowkey = "r";
    Cell cell(CellType::CT_PUT, rowkey, "", "", "", 100);
    EXPECT_FALSE(cell.empty()); // rowkey is non-empty
    EXPECT_EQ(cell.total_size(), 1u);
}

// ==================== Cell::reset ====================

TEST_F(CellTest, reset_clears_all) {
    std::string rowkey = "row";
    std::string cf = "cf";
    std::string col = "col";
    std::string val = "value";
    Cell cell(CellType::CT_PUT, rowkey, cf, col, val, 100);
    EXPECT_FALSE(cell.empty());

    cell.reset();
    EXPECT_TRUE(cell.empty());
    EXPECT_EQ(cell.total_size(), 0u);
    EXPECT_EQ(cell.cell_type(), CellType::CT_NONE);
    EXPECT_EQ(cell.timestamp(), 0u);
}

// ==================== CellKey::reset ====================

TEST_F(CellTest, cellkey_reset) {
    CellKey ck("row", "cf", "col", 999, CellType::CT_PUT);
    ck.reset();
    EXPECT_EQ(ck.rowkey, std::string_view());
    EXPECT_EQ(ck.cf, std::string_view());
    EXPECT_EQ(ck.col, std::string_view());
    EXPECT_EQ(ck.timestamp, 0u);
    EXPECT_EQ(ck.cell_type, CellType::CT_NONE);
}

} // namespace pl
