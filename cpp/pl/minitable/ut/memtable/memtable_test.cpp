// Copyright (c) 2026 The Authors. All rights reserved.
#include "cpp/pl/minitable/memtable/memtable.h"

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace pl::minitable {
namespace {

TEST(MemTableTest, OrdersKeysAndSupportsLowerBound) {
    auto table = MemTable::Create();
    ASSERT_TRUE(table.ok());
    EXPECT_TRUE((*table)->put("c", "3", 1).ok());
    EXPECT_TRUE((*table)->put("a", "1", 2).ok());
    EXPECT_TRUE((*table)->put("b", "2", 3).ok());

    auto cursor = (*table)->new_cursor();
    ASSERT_TRUE(cursor->seek_to_first().ok());
    std::vector<std::string> keys;
    while (cursor->valid()) {
        keys.emplace_back(cursor->key());
        ASSERT_TRUE(cursor->next().ok());
    }
    EXPECT_EQ(keys, (std::vector<std::string>{"a", "b", "c"}));

    cursor = (*table)->new_cursor();
    ASSERT_TRUE(cursor->seek("bb").ok());
    ASSERT_TRUE(cursor->valid());
    EXPECT_EQ(cursor->key(), "c");
}

TEST(MemTableTest, FreezeIsIdempotentAndRejectsWrites) {
    auto table = MemTable::Create();
    ASSERT_TRUE(table.ok());
    ASSERT_TRUE((*table)->put("a", "1", 10).ok());
    EXPECT_TRUE((*table)->freeze().ok());
    EXPECT_TRUE((*table)->freeze().ok());
    EXPECT_TRUE((*table)->frozen());
    EXPECT_TRUE((*table)->should_flush());
    EXPECT_EQ((*table)->put("b", "2", 11).code(), absl::StatusCode::kFailedPrecondition);

    auto cursor = (*table)->new_cursor();
    ASSERT_TRUE(cursor->seek_to_first().ok());
    ASSERT_TRUE(cursor->valid());
    EXPECT_EQ(cursor->value(), "1");
}

TEST(MemTableTest, EnforcesApplyOrderAndMemoryLimitWithoutPartialInsert) {
    auto table = MemTable::Create(
        {.memory_limit_bytes = 64, .arena_block_bytes = 16});
    ASSERT_TRUE(table.ok());
    ASSERT_TRUE((*table)->put("a", "value", 5).ok());
    EXPECT_EQ((*table)->put("b", "value", 4).code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ((*table)->put("large", std::string(64, 'x'), 6).code(),
              absl::StatusCode::kResourceExhausted);
    EXPECT_EQ((*table)->size(), 1U);
    EXPECT_EQ((*table)->max_apply_index(), 5U);
}

TEST(MemTableTest, ReplacementKeepsSingleOrderedEntry) {
    auto table = MemTable::Create();
    ASSERT_TRUE(table.ok());
    ASSERT_TRUE((*table)->put("a", "old", 1).ok());
    ASSERT_TRUE((*table)->put("a", "new", 2).ok());
    EXPECT_EQ((*table)->size(), 1U);
    EXPECT_EQ((*table)->max_apply_index(), 2U);

    auto cursor = (*table)->new_cursor();
    ASSERT_TRUE(cursor->seek_to_first().ok());
    ASSERT_TRUE(cursor->valid());
    EXPECT_EQ(cursor->value(), "new");
}

TEST(MemTableTest, ValidatesConfigurationAndEmptyKey) {
    EXPECT_FALSE(MemTable::Create({.memory_limit_bytes = 0}).ok());
    EXPECT_FALSE(MemTable::Create({.memory_limit_bytes = 8, .arena_block_bytes = 16}).ok());
    auto table = MemTable::Create();
    ASSERT_TRUE(table.ok());
    EXPECT_EQ((*table)->put("", "v", 1).code(), absl::StatusCode::kInvalidArgument);
}

} // namespace
} // namespace pl::minitable
