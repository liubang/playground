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

TEST(MemTableTest, BatchPublishesAllMutationsAtOneApplyIndex) {
    auto table = MemTable::Create();
    ASSERT_TRUE(table.ok());
    const std::vector<MemTableMutation> mutations = {
        {.encoded_key = "c", .encoded_value = "3"},
        {.encoded_key = "a", .encoded_value = "old"},
        {.encoded_key = "b", .encoded_value = "2"},
        {.encoded_key = "a", .encoded_value = "1"},
    };
    ASSERT_TRUE((*table)->put_batch(mutations, 7).ok());
    EXPECT_EQ((*table)->size(), 3U);
    EXPECT_EQ((*table)->max_apply_index(), 7U);

    auto cursor = (*table)->new_cursor();
    ASSERT_TRUE(cursor->seek_to_first().ok());
    std::vector<std::string> values;
    while (cursor->valid()) {
        values.emplace_back(cursor->value());
        ASSERT_TRUE(cursor->next().ok());
    }
    EXPECT_EQ(values, (std::vector<std::string>{"1", "2", "3"}));
}

TEST(MemTableTest, BatchValidationFailureDoesNotPublishPartialChanges) {
    auto table = MemTable::Create({.memory_limit_bytes = 48, .arena_block_bytes = 4});
    ASSERT_TRUE(table.ok());
    ASSERT_TRUE((*table)->put("a", "old", 3).ok());

    const std::vector<MemTableMutation> oversized = {
        {.encoded_key = "a", .encoded_value = "new"},
        {.encoded_key = "b", .encoded_value = "12345"},
    };
    EXPECT_EQ((*table)->put_batch(oversized, 4).code(),
              absl::StatusCode::kResourceExhausted);
    EXPECT_EQ((*table)->size(), 1U);
    EXPECT_EQ((*table)->max_apply_index(), 3U);

    auto cursor = (*table)->new_cursor();
    ASSERT_TRUE(cursor->seek_to_first().ok());
    ASSERT_TRUE(cursor->valid());
    EXPECT_EQ(cursor->key(), "a");
    EXPECT_EQ(cursor->value(), "old");

    const std::vector<MemTableMutation> invalid = {
        {.encoded_key = "c", .encoded_value = "3"},
        {.encoded_key = "", .encoded_value = "invalid"},
    };
    EXPECT_EQ((*table)->put_batch(invalid, 4).code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ((*table)->size(), 1U);
    EXPECT_EQ((*table)->max_apply_index(), 3U);
}

TEST(MemTableTest, BatchChargesOnlyFinalValueForDuplicateKeys) {
    auto table = MemTable::Create({.memory_limit_bytes = 80, .arena_block_bytes = 4});
    ASSERT_TRUE(table.ok());
    const std::vector<MemTableMutation> mutations = {
        {.encoded_key = "a", .encoded_value = "value-too-large"},
        {.encoded_key = "a", .encoded_value = "1"},
        {.encoded_key = "b", .encoded_value = "2"},
    };
    EXPECT_TRUE((*table)->put_batch(mutations, 1).ok());
    EXPECT_EQ((*table)->size(), 2U);
}

TEST(MemTableTest, PreparedBatchIsInvisibleUntilPublish) {
    auto table = MemTable::Create();
    ASSERT_TRUE(table.ok());
    ASSERT_TRUE((*table)->put("a", "old", 1).ok());
    const std::vector<MemTableMutation> mutations = {
        {.encoded_key = "a", .encoded_value = "new"},
        {.encoded_key = "b", .encoded_value = "2"},
    };

    auto prepared = (*table)->prepare_batch(mutations, 2);
    ASSERT_TRUE(prepared.ok());
    EXPECT_EQ((*table)->size(), 1U);
    EXPECT_EQ((*table)->max_apply_index(), 1U);
    prepared->publish();
    EXPECT_EQ((*table)->size(), 2U);
    EXPECT_EQ((*table)->max_apply_index(), 2U);
}

TEST(MemTableTest, AbortedBatchRewindsReservationAndCanBeRetried) {
    auto table = MemTable::Create({.memory_limit_bytes = 80, .arena_block_bytes = 8});
    ASSERT_TRUE(table.ok());
    const std::vector<MemTableMutation> mutations = {
        {.encoded_key = "a", .encoded_value = "1"},
        {.encoded_key = "b", .encoded_value = "2"},
    };
    {
        auto prepared = (*table)->prepare_batch(mutations, 1);
        ASSERT_TRUE(prepared.ok());
        prepared->abort();
    }
    EXPECT_EQ((*table)->size(), 0U);
    EXPECT_EQ((*table)->max_apply_index(), 0U);
    EXPECT_TRUE((*table)->put_batch(mutations, 1).ok());
}

TEST(MemTableTest, CursorUsesPinnedVisibilityWatermarkAcrossOverwrites) {
    auto table = MemTable::Create();
    ASSERT_TRUE(table.ok());
    const std::vector<MemTableMutation> first = {
        {.encoded_key = "a", .encoded_value = "v1"},
        {.encoded_key = "b", .encoded_value = "v1"},
    };
    const std::vector<MemTableMutation> second = {
        {.encoded_key = "a", .encoded_value = "v2"},
        {.encoded_key = "c", .encoded_value = "v2"},
    };
    ASSERT_TRUE((*table)->put_batch(first, 1).ok());
    ASSERT_TRUE((*table)->put_batch(second, 2).ok());

    auto old_cursor = (*table)->new_cursor(1);
    ASSERT_TRUE(old_cursor->seek_to_first().ok());
    ASSERT_TRUE(old_cursor->valid());
    EXPECT_EQ(old_cursor->key(), "a");
    EXPECT_EQ(old_cursor->value(), "v1");
    ASSERT_TRUE(old_cursor->next().ok());
    ASSERT_TRUE(old_cursor->valid());
    EXPECT_EQ(old_cursor->key(), "b");
    EXPECT_EQ(old_cursor->value(), "v1");
    ASSERT_TRUE(old_cursor->next().ok());
    EXPECT_FALSE(old_cursor->valid());

    auto current_cursor = (*table)->new_cursor(2);
    ASSERT_TRUE(current_cursor->seek("a").ok());
    ASSERT_TRUE(current_cursor->valid());
    EXPECT_EQ(current_cursor->value(), "v2");
}

TEST(MemTableTest, PreparedBatchSafelyOutlivesLastExternalTableOwner) {
    const std::vector<MemTableMutation> mutations = {
        {.encoded_key = "a", .encoded_value = "1"}};
    {
        auto table = MemTable::Create();
        ASSERT_TRUE(table.ok());
        auto prepared = (*table)->prepare_batch(mutations, 1);
        ASSERT_TRUE(prepared.ok());
        table->reset();
        prepared->publish();
    }
    {
        auto table = MemTable::Create();
        ASSERT_TRUE(table.ok());
        auto prepared = (*table)->prepare_batch(mutations, 1);
        ASSERT_TRUE(prepared.ok());
        table->reset();
        prepared->abort();
    }
}

TEST(MemTableTest, RejectsZeroAndDuplicateApplyIndexes) {
    auto table = MemTable::Create();
    ASSERT_TRUE(table.ok());
    EXPECT_EQ((*table)->put("a", "zero", 0).code(), absl::StatusCode::kInvalidArgument);
    ASSERT_TRUE((*table)->put("a", "one", 1).ok());
    EXPECT_EQ((*table)->put("a", "duplicate", 1).code(),
              absl::StatusCode::kInvalidArgument);

    auto cursor = (*table)->new_cursor(1);
    ASSERT_TRUE(cursor->seek_to_first().ok());
    ASSERT_TRUE(cursor->valid());
    EXPECT_EQ(cursor->value(), "one");
}

TEST(MemTableTest, PendingPrepareRejectsOtherWritersAndFreeze) {
    auto table = MemTable::Create();
    ASSERT_TRUE(table.ok());
    const std::vector<MemTableMutation> mutations = {
        {.encoded_key = "a", .encoded_value = "1"}};
    auto prepared = (*table)->prepare_batch(mutations, 1);
    ASSERT_TRUE(prepared.ok());

    EXPECT_EQ((*table)->put("b", "2", 1).code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ((*table)->prepare_batch(mutations, 1).status().code(),
              absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ((*table)->freeze().code(), absl::StatusCode::kFailedPrecondition);
    prepared->abort();
    EXPECT_TRUE((*table)->put("b", "2", 1).ok());
}

TEST(MemTableTest, PreparedBatchDestructorImplicitlyAborts) {
    auto table = MemTable::Create();
    ASSERT_TRUE(table.ok());
    const std::vector<MemTableMutation> mutations = {
        {.encoded_key = "a", .encoded_value = "1"}};
    {
        auto prepared = (*table)->prepare_batch(mutations, 1);
        ASSERT_TRUE(prepared.ok());
    }
    EXPECT_EQ((*table)->size(), 0U);
    EXPECT_TRUE((*table)->put_batch(mutations, 1).ok());
}

TEST(MemTableTest, ValidatesConfigurationEmptyKeyAndEmptyBatch) {
    EXPECT_FALSE(MemTable::Create({.memory_limit_bytes = 0}).ok());
    EXPECT_FALSE(MemTable::Create({.memory_limit_bytes = 8, .arena_block_bytes = 16}).ok());
    auto table = MemTable::Create();
    ASSERT_TRUE(table.ok());
    EXPECT_EQ((*table)->put("", "v", 1).code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ((*table)->put_batch({}, 1).code(), absl::StatusCode::kInvalidArgument);
}

} // namespace
} // namespace pl::minitable
