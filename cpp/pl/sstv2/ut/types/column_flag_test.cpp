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

#include "cpp/pl/sstv2/types/column_flag.h"

namespace pl::sstv2::types {
namespace {

TEST(ColumnFlagTest, DefaultIsZero) {
    ColumnFlag flag;
    EXPECT_EQ(flag.raw(), 0u);
    EXPECT_EQ(flag.data_type(), DataType::kNone);
    EXPECT_FALSE(flag.has_checksum());
    EXPECT_FALSE(flag.bool_value());
}

TEST(ColumnFlagTest, ForValue) {
    auto flag = ColumnFlag::for_value(DataType::kString, true);
    EXPECT_EQ(flag.data_type(), DataType::kString);
    EXPECT_TRUE(flag.has_checksum());
    EXPECT_FALSE(flag.bool_value());
    EXPECT_TRUE(flag.is_value_flag());
    EXPECT_FALSE(flag.is_index_entry());
    EXPECT_TRUE(flag.is_valid());
}

TEST(ColumnFlagTest, BoolValueBit) {
    auto flag_true = ColumnFlag::for_value(DataType::kBool, false, true);
    EXPECT_EQ(flag_true.data_type(), DataType::kBool);
    EXPECT_FALSE(flag_true.has_checksum());
    EXPECT_TRUE(flag_true.bool_value());
    EXPECT_TRUE(flag_true.is_valid());

    auto flag_false = ColumnFlag::for_value(DataType::kBool, true, false);
    EXPECT_TRUE(flag_false.has_checksum());
    EXPECT_FALSE(flag_false.bool_value());
    EXPECT_TRUE(flag_false.is_valid());
}

TEST(ColumnFlagTest, DataBlockPtr) {
    auto flag = ColumnFlag::for_data_block();
    EXPECT_EQ(flag.data_type(), DataType::kDataBlock);
    EXPECT_TRUE(flag.is_index_entry());
    EXPECT_TRUE(flag.is_data_block_ptr());
    EXPECT_FALSE(flag.is_index_block_ptr());
    EXPECT_FALSE(flag.is_value_flag());
    EXPECT_EQ(flag.raw(), 21u);
    EXPECT_TRUE(flag.is_valid());
}

TEST(ColumnFlagTest, IndexBlockPtr) {
    auto flag = ColumnFlag::for_index_block();
    EXPECT_EQ(flag.data_type(), DataType::kIndexBlock);
    EXPECT_TRUE(flag.is_index_entry());
    EXPECT_FALSE(flag.is_data_block_ptr());
    EXPECT_TRUE(flag.is_index_block_ptr());
    EXPECT_EQ(flag.raw(), 22u);
    EXPECT_TRUE(flag.is_valid());
}

TEST(ColumnFlagTest, RawRoundTrip) {
    auto original = ColumnFlag::for_value(DataType::kUint64, true);
    auto restored = ColumnFlag::from_raw(original.raw());
    EXPECT_EQ(restored, original);
}

TEST(ColumnFlagTest, Equality) {
    auto a = ColumnFlag::for_value(DataType::kInt32, true);
    auto b = ColumnFlag::for_value(DataType::kInt32, true);
    auto c = ColumnFlag::for_value(DataType::kInt32, false);
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

TEST(ColumnFlagTest, BitLayout) {
    // DT=9 (kUint64) in bits 0-7, C=1 in bit 8, B=0 in bit 9
    auto flag = ColumnFlag::for_value(DataType::kUint64, true, false);
    // Expected: 9 | (1<<8) = 265
    EXPECT_EQ(flag.raw(), 265u);

    // DT=1 (kBool), C=1, B=1
    auto flag2 = ColumnFlag::for_value(DataType::kBool, true, true);
    // Expected: 1 | (1<<8) | (1<<9) = 769
    EXPECT_EQ(flag2.raw(), 769u);
}

TEST(ColumnFlagTest, ValidationRejectsReservedBits) {
    // Set a reserved bit (bit 10).
    auto bad = ColumnFlag::from_raw(1ULL << 10);
    EXPECT_FALSE(bad.is_valid());
}

TEST(ColumnFlagTest, ValidationRejectsIndexEntryWithChecksum) {
    // Index entry with checksum bit set is invalid.
    uint64_t raw = static_cast<uint8_t>(DataType::kDataBlock) | ColumnFlag::kChecksumBit;
    auto bad = ColumnFlag::from_raw(raw);
    EXPECT_FALSE(bad.is_valid());
}

TEST(ColumnFlagTest, ValidationRejectsBoolBitOnNonBool) {
    // Bool bit set on a non-Bool type is invalid.
    uint64_t raw = static_cast<uint8_t>(DataType::kInt32) | ColumnFlag::kBoolBit;
    auto bad = ColumnFlag::from_raw(raw);
    EXPECT_FALSE(bad.is_valid());
}

TEST(ColumnFlagTest, ConstexprUsable) {
    // Verify constexpr construction works at compile time.
    constexpr auto flag = ColumnFlag::for_value(DataType::kDouble, false);
    static_assert(flag.data_type() == DataType::kDouble);
    static_assert(!flag.has_checksum());
    static_assert(flag.is_valid());
    static_assert(flag.is_value_flag());
}

} // namespace
} // namespace pl::sstv2::types
