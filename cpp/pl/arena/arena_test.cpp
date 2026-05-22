// Copyright (c) 2022 The Authors. All rights reserved.
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
// Created: 2022/01/05 20:44

#include <gtest/gtest.h>
#include <utility>

#include "cpp/pl/arena/arena.h"

namespace {
struct TestObject {
    int value = 0;
    std::string name;

    TestObject(int v, std::string n) : value(v), name(std::move(n)) {}

    ~TestObject() = default;
};

struct ThrowingObject {
    ThrowingObject() { throw std::runtime_error("construction failed"); }
};

constexpr std::size_t align_up(std::size_t size, std::size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

} // namespace

TEST(ArenaTest, BasicAllocation) {
    pl::Arena arena(1024);
    void* raw_ptr = arena.allocate(100);
    ASSERT_NE(nullptr, raw_ptr);
    auto stats = arena.get_stats();
    ASSERT_EQ(1, stats.block_count);
    ASSERT_EQ(1024, stats.total_allocated);
    ASSERT_EQ(align_up(100, alignof(std::max_align_t)), stats.total_used);
}

TEST(ArenaTest, AllocateObject) {
    pl::Arena arena(1024);
    auto* obj = arena.allocate_object<TestObject>(42, "Hello world");
    ASSERT_NE(nullptr, obj);
    ASSERT_EQ(42, obj->value);
    ASSERT_EQ("Hello world", obj->name);

    auto stats = arena.get_stats();
    ASSERT_EQ(sizeof(TestObject), stats.total_used);
}

TEST(ArenaTest, AllocateZeroSize) {
    pl::Arena arena(1024);
    void* ptr = arena.allocate(0);
    ASSERT_EQ(nullptr, ptr);

    // Arena should still have one block from construction, but no usage
    auto stats = arena.get_stats();
    ASSERT_EQ(1, stats.block_count);
    ASSERT_EQ(0, stats.total_used);
}

TEST(ArenaTest, AllocateExceedsCurrentBlock) {
    pl::Arena arena(64);

    // First allocation fits in initial block
    void* ptr1 = arena.allocate(32);
    ASSERT_NE(nullptr, ptr1);

    // Second allocation exceeds remaining space, triggers new block
    void* ptr2 = arena.allocate(64);
    ASSERT_NE(nullptr, ptr2);

    auto stats = arena.get_stats();
    ASSERT_EQ(2, stats.block_count);
    ASSERT_EQ(128, stats.total_allocated); // 64 + 64
}

TEST(ArenaTest, AllocateLargerThanDefaultBlock) {
    pl::Arena arena(64);

    // Allocate something larger than default block size
    void* ptr = arena.allocate(256);
    ASSERT_NE(nullptr, ptr);

    auto stats = arena.get_stats();
    ASSERT_EQ(2, stats.block_count);
    // First block is 64, second block is 256 (max of default and requested)
    ASSERT_EQ(64 + 256, stats.total_allocated);
}

TEST(ArenaTest, Reset) {
    pl::Arena arena(1024);

    arena.allocate(100);
    arena.allocate(200);
    arena.allocate(2048); // triggers new block

    auto stats = arena.get_stats();
    ASSERT_EQ(2, stats.block_count);

    arena.reset();

    stats = arena.get_stats();
    ASSERT_EQ(1, stats.block_count);
    ASSERT_EQ(0, stats.total_used);
    ASSERT_EQ(1024, stats.total_allocated);
}

TEST(ArenaTest, AvailableInCurrentBlock) {
    pl::Arena arena(1024);
    ASSERT_EQ(1024, arena.available_in_current_block());

    arena.allocate(100);
    std::size_t used = align_up(100, alignof(std::max_align_t));
    ASSERT_EQ(1024 - used, arena.available_in_current_block());
}

TEST(ArenaTest, FragmentationRatio) {
    pl::Arena arena(1024);

    // Use half the block
    arena.allocate(512);

    auto stats = arena.get_stats();
    ASSERT_GT(stats.fragmentation_ratio, 0.0);
    ASSERT_LT(stats.fragmentation_ratio, 1.0);

    double expected = 1.0 - (static_cast<double>(stats.total_used) / stats.total_allocated);
    ASSERT_DOUBLE_EQ(expected, stats.fragmentation_ratio);
}

TEST(ArenaTest, AllocateObjectThrowsOnConstruction) {
    pl::Arena arena(1024);
    ASSERT_THROW(arena.allocate_object<ThrowingObject>(), std::runtime_error);
}

TEST(ArenaTest, MultipleAllocationsAlignment) {
    pl::Arena arena(1024);

    // Allocate with different alignments
    void* ptr1 = arena.allocate(1, 1);
    void* ptr2 = arena.allocate(1, 16);
    void* ptr3 = arena.allocate(1, 64);

    ASSERT_NE(nullptr, ptr1);
    ASSERT_NE(nullptr, ptr2);
    ASSERT_NE(nullptr, ptr3);

    // Verify alignment
    ASSERT_EQ(0, reinterpret_cast<std::uintptr_t>(ptr2) % 16);
    ASSERT_EQ(0, reinterpret_cast<std::uintptr_t>(ptr3) % 64);
}

TEST(ArenaTest, AllocateAfterReset) {
    pl::Arena arena(1024);

    void* ptr1 = arena.allocate(100);
    ASSERT_NE(nullptr, ptr1);

    arena.reset();

    // After reset, should be able to allocate from the beginning again
    void* ptr2 = arena.allocate(100);
    ASSERT_NE(nullptr, ptr2);
    ASSERT_EQ(ptr1, ptr2); // Should reuse the same memory

    auto stats = arena.get_stats();
    ASSERT_EQ(1, stats.block_count);
}

TEST(ArenaTest, ExhaustCurrentBlockThenAllocate) {
    pl::Arena arena(128);

    // Fill up the block completely
    arena.allocate(128);
    ASSERT_EQ(0, arena.available_in_current_block());

    // Next allocation must go to a new block
    void* ptr = arena.allocate(64);
    ASSERT_NE(nullptr, ptr);

    auto stats = arena.get_stats();
    ASSERT_EQ(2, stats.block_count);
}
