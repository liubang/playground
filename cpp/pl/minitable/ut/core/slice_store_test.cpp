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
// Created: 2026/07/18 00:36

#include "cpp/pl/minitable/core/slice_store.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace pl::minitable {
namespace {

std::string ReadValue(const SliceReadView& view, uint32_t locality_group_id, std::string_view key) {
    auto cursor = view.new_cursor(locality_group_id);
    EXPECT_TRUE(cursor.ok());
    if (!cursor.ok()) {
        return {};
    }
    EXPECT_TRUE((*cursor)->seek(key).ok());
    if (!(*cursor)->valid() || (*cursor)->key() != key) {
        return {};
    }
    return std::string((*cursor)->value());
}

TEST(SliceStoreTest, AppliesMultipleLocalityGroupsAtOneVisibilityPoint) {
    auto store = SliceStore::Create({{1, {}}, {2, {}}});
    ASSERT_TRUE(store.ok());

    const std::vector<MemTableMutation> first = {
        {.encoded_key = "row/cf1", .encoded_value = "one"}};
    const std::vector<MemTableMutation> second = {
        {.encoded_key = "row/cf2", .encoded_value = "two"}};
    const std::vector<LocalityGroupPatch> patches = {
        {.locality_group_id = 2, .mutations = second},
        {.locality_group_id = 1, .mutations = first},
    };

    ASSERT_TRUE((*store)->apply(patches, 7).ok());
    const auto view = (*store)->read_view();
    EXPECT_EQ(view.visible_applied_index(), 7U);
    EXPECT_EQ(ReadValue(view, 1, "row/cf1"), "one");
    EXPECT_EQ(ReadValue(view, 2, "row/cf2"), "two");
}

TEST(SliceStoreTest, PrepareFailureAbortsEveryLocalityGroup) {
    auto store = SliceStore::Create(
        {{1, {.memory_limit_bytes = 128, .arena_block_bytes = 8}},
         {2, {.memory_limit_bytes = 32, .arena_block_bytes = 8}}});
    ASSERT_TRUE(store.ok());

    const std::vector<MemTableMutation> first = {
        {.encoded_key = "a", .encoded_value = "accepted"}};
    const std::vector<MemTableMutation> second = {
        {.encoded_key = "b", .encoded_value = std::string(64, 'x')}};
    const std::vector<LocalityGroupPatch> patches = {
        {.locality_group_id = 1, .mutations = first},
        {.locality_group_id = 2, .mutations = second},
    };

    EXPECT_EQ((*store)->apply(patches, 1).code(), absl::StatusCode::kResourceExhausted);
    EXPECT_EQ((*store)->visible_applied_index(), 0U);
    EXPECT_TRUE(ReadValue((*store)->read_view(), 1, "a").empty());
    EXPECT_TRUE(ReadValue((*store)->read_view(), 2, "b").empty());

    const std::vector<LocalityGroupPatch> retry = {
        {.locality_group_id = 1, .mutations = first}};
    EXPECT_TRUE((*store)->apply(retry, 1).ok());
}

TEST(SliceStoreTest, PinnedReadViewRetainsOldCrossLocalityGroupState) {
    auto store = SliceStore::Create({{1, {}}, {2, {}}});
    ASSERT_TRUE(store.ok());

    const std::vector<MemTableMutation> lg1v1 = {{.encoded_key = "k", .encoded_value = "1a"}};
    const std::vector<MemTableMutation> lg2v1 = {{.encoded_key = "k", .encoded_value = "1b"}};
    const std::vector<LocalityGroupPatch> first = {
        {.locality_group_id = 1, .mutations = lg1v1},
        {.locality_group_id = 2, .mutations = lg2v1},
    };
    ASSERT_TRUE((*store)->apply(first, 1).ok());
    const auto old_view = (*store)->read_view();

    const std::vector<MemTableMutation> lg1v2 = {{.encoded_key = "k", .encoded_value = "2a"}};
    const std::vector<MemTableMutation> lg2v2 = {{.encoded_key = "k", .encoded_value = "2b"}};
    const std::vector<LocalityGroupPatch> second = {
        {.locality_group_id = 1, .mutations = lg1v2},
        {.locality_group_id = 2, .mutations = lg2v2},
    };
    ASSERT_TRUE((*store)->apply(second, 2).ok());
    const auto current_view = (*store)->read_view();

    EXPECT_EQ(ReadValue(old_view, 1, "k"), "1a");
    EXPECT_EQ(ReadValue(old_view, 2, "k"), "1b");
    EXPECT_EQ(ReadValue(current_view, 1, "k"), "2a");
    EXPECT_EQ(ReadValue(current_view, 2, "k"), "2b");
}

TEST(SliceStoreTest, ConcurrentReadViewsNeverObserveMixedApplyIndexes) {
    auto store = SliceStore::Create(
        {{1, {.memory_limit_bytes = 64 * 1024, .arena_block_bytes = 256}},
         {2, {.memory_limit_bytes = 64 * 1024, .arena_block_bytes = 256}}});
    ASSERT_TRUE(store.ok());

    std::atomic<uint64_t> requested_index = 0;
    std::atomic<uint64_t> observed_index = 0;
    std::atomic<bool> mixed = false;
    std::thread reader([&] {
        for (uint64_t expected = 1; expected <= 100; ++expected) {
            while (requested_index.load(std::memory_order_acquire) < expected) {
                std::this_thread::yield();
            }
            const auto view = (*store)->read_view();
            const auto first = ReadValue(view, 1, "k");
            const auto second = ReadValue(view, 2, "k");
            if (view.visible_applied_index() != expected || first != second ||
                first != std::to_string(expected)) {
                mixed.store(true, std::memory_order_release);
                return;
            }
            observed_index.store(expected, std::memory_order_release);
        }
    });

    for (uint64_t index = 1; index <= 100; ++index) {
        const std::string value = std::to_string(index);
        const std::vector<MemTableMutation> first = {
            {.encoded_key = "k", .encoded_value = value}};
        const std::vector<MemTableMutation> second = {
            {.encoded_key = "k", .encoded_value = value}};
        const std::vector<LocalityGroupPatch> patches = {
            {.locality_group_id = 1, .mutations = first},
            {.locality_group_id = 2, .mutations = second},
        };
        const auto status = (*store)->apply(patches, index);
        if (!status.ok()) {
            mixed.store(true, std::memory_order_release);
            requested_index.store(100, std::memory_order_release);
            break;
        }
        requested_index.store(index, std::memory_order_release);
        while (observed_index.load(std::memory_order_acquire) < index &&
               !mixed.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    reader.join();
    EXPECT_EQ(observed_index.load(std::memory_order_acquire), 100U);
    EXPECT_FALSE(mixed.load(std::memory_order_acquire));
}

TEST(SliceStoreTest, RejectsInvalidPatchSetsAndNonAdvancingIndex) {
    auto store = SliceStore::Create({{1, {}}});
    ASSERT_TRUE(store.ok());
    const std::vector<MemTableMutation> mutations = {{.encoded_key = "k", .encoded_value = "v"}};
    const std::vector<LocalityGroupPatch> valid = {{.locality_group_id = 1,
                                                    .mutations = mutations}};
    ASSERT_TRUE((*store)->apply(valid, 1).ok());
    EXPECT_EQ((*store)->apply(valid, 1).code(), absl::StatusCode::kInvalidArgument);

    const std::vector<LocalityGroupPatch> duplicate = {
        {.locality_group_id = 1, .mutations = mutations},
        {.locality_group_id = 1, .mutations = mutations},
    };
    EXPECT_EQ((*store)->apply(duplicate, 2).code(), absl::StatusCode::kInvalidArgument);
    EXPECT_FALSE((*store)->read_view().new_cursor(99).ok());
}

} // namespace
} // namespace pl::minitable
