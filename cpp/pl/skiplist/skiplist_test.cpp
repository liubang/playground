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
// Created: 2024/12/09 00:05

#include <atomic>
#include <gtest/gtest.h>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "cpp/pl/skiplist/skiplist.h"

namespace pl {
namespace {

// =========================================================================
// Comparator helpers
// =========================================================================

struct IntCmp {
    [[nodiscard]] int compare(const int& a, const int& b) const {
        if (a < b) {
            return -1;
        }
        if (a > b) {
            return 1;
        }
        return 0;
    }
};

struct StringCmp {
    [[nodiscard]] int compare(const std::string& a, const std::string& b) const {
        if (a < b) {
            return -1;
        }
        if (a > b) {
            return 1;
        }
        return 0;
    }
};

using IntStringList = SkipList<int, std::string, IntCmp>;

// =========================================================================
// Basic insert / find / contains
// =========================================================================

TEST(SkipListTest, InsertAndFind) {
    IntStringList list;

    list.insert(5, "five");
    list.insert(3, "three");
    list.insert(7, "seven");
    list.insert(1, "one");

    EXPECT_EQ(list.size(), 4);
    EXPECT_FALSE(list.empty());

    const auto* n = list.find_first_gte(5);
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->key, 5);
    EXPECT_EQ(n->value, "five");

    n = list.find_first_gte(4);
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->key, 5); // first >= 4 is 5

    n = list.find_first_gte(0);
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->key, 1); // first >= 0 is 1

    n = list.find_first_gte(8);
    EXPECT_EQ(n, nullptr); // nothing >= 8
}

TEST(SkipListTest, Contains) {
    IntStringList list;
    list.insert(5, "five");
    list.insert(3, "three");

    EXPECT_TRUE(list.contains(5));
    EXPECT_TRUE(list.contains(3));
    EXPECT_FALSE(list.contains(4));
    EXPECT_FALSE(list.contains(0));
}

TEST(SkipListTest, FindFirstGt) {
    IntStringList list;
    list.insert(1, "a");
    list.insert(3, "c");
    list.insert(5, "e");

    const auto* n = list.find_first_gt(3);
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->key, 5);

    n = list.find_first_gt(0);
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->key, 1);

    n = list.find_first_gt(5);
    EXPECT_EQ(n, nullptr);
}

// =========================================================================
// Iteration
// =========================================================================

TEST(SkipListTest, OrderedIteration) {
    IntStringList list;

    // Insert in reverse order
    std::vector<int> keys = {10, 1, 8, 3, 6, 5, 7, 2, 9, 4};
    for (int k : keys) {
        list.insert(k, "val_" + std::to_string(k));
    }

    // Iterate — must be in sorted order
    int prev = -1;
    for (auto it = list.begin(); it != list.end(); ++it) {
        EXPECT_GT(it->key, prev);
        prev = it->key;
        EXPECT_EQ(it->value, "val_" + std::to_string(it->key));
    }
    EXPECT_EQ(prev, 10);
}

TEST(SkipListTest, LowerBound) {
    IntStringList list;
    list.insert(10, "ten");
    list.insert(20, "twenty");
    list.insert(30, "thirty");

    auto it = list.lower_bound(15);
    ASSERT_NE(it, list.end());
    EXPECT_EQ(it->key, 20);

    it = list.lower_bound(20);
    ASSERT_NE(it, list.end());
    EXPECT_EQ(it->key, 20);

    it = list.lower_bound(35);
    EXPECT_EQ(it, list.end());

    it = list.lower_bound(0);
    ASSERT_NE(it, list.end());
    EXPECT_EQ(it->key, 10);
}

TEST(SkipListTest, RangeScan) {
    IntStringList list;
    for (int i = 1; i <= 100; ++i) {
        list.insert(i, "v" + std::to_string(i));
    }

    // Scan [25, 50)
    int count = 0;
    auto it = list.lower_bound(25);
    for (; it != list.end() && it->key < 50; ++it) {
        EXPECT_GE(it->key, 25);
        EXPECT_LT(it->key, 50);
        ++count;
    }
    EXPECT_EQ(count, 25);
}

// =========================================================================
// Empty list
// =========================================================================

TEST(SkipListTest, EmptyList) {
    IntStringList list;

    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.size(), 0);
    EXPECT_FALSE(list.contains(1));

    EXPECT_EQ(list.find_first_gte(0), nullptr);
    EXPECT_EQ(list.find_first_gt(0), nullptr);
    EXPECT_EQ(list.begin(), list.end());
    EXPECT_EQ(list.lower_bound(0), list.end());
}

// =========================================================================
// Duplicate keys (FIFO — new dups after older ones)
// =========================================================================

TEST(SkipListTest, DuplicateKeys) {
    IntStringList list;

    list.insert(1, "first");
    list.insert(1, "second");
    list.insert(1, "third");

    EXPECT_EQ(list.size(), 3);

    // find_first_gte returns the first (oldest)
    const auto* n = list.find_first_gte(1);
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->key, 1);
    EXPECT_EQ(n->value, "first");

    // Iterate to see all three in insertion order
    std::vector<std::string> vals;
    for (auto it = list.lower_bound(1); it != list.end() && it->key == 1; ++it) {
        vals.push_back(it->value);
    }
    ASSERT_EQ(vals.size(), 3);
    EXPECT_EQ(vals[0], "first");
    EXPECT_EQ(vals[1], "second");
    EXPECT_EQ(vals[2], "third");
}

// =========================================================================
// Remove
// =========================================================================

TEST(SkipListTest, Remove) {
    IntStringList list;
    list.insert(1, "a");
    list.insert(2, "b");
    list.insert(3, "c");
    list.insert(4, "d");
    list.insert(5, "e");
    EXPECT_EQ(list.size(), 5);

    // Remove middle
    EXPECT_TRUE(list.remove(3));
    EXPECT_EQ(list.size(), 4);
    EXPECT_FALSE(list.contains(3));

    // Remove head
    EXPECT_TRUE(list.remove(1));
    EXPECT_EQ(list.size(), 3);
    EXPECT_FALSE(list.contains(1));

    // Remove tail
    EXPECT_TRUE(list.remove(5));
    EXPECT_EQ(list.size(), 2);
    EXPECT_FALSE(list.contains(5));

    // Remove non-existent
    EXPECT_FALSE(list.remove(999));
    EXPECT_EQ(list.size(), 2);

    // Remaining elements still accessible
    EXPECT_TRUE(list.contains(2));
    EXPECT_TRUE(list.contains(4));

    // Verify order
    std::vector<int> remaining;
    for (const auto& node : list) {
        remaining.push_back(node.key);
    }
    ASSERT_EQ(remaining.size(), 2);
    EXPECT_EQ(remaining[0], 2);
    EXPECT_EQ(remaining[1], 4);
}

TEST(SkipListTest, RemoveAllToOneByOne) {
    IntStringList list;
    for (int i = 0; i < 50; ++i) {
        list.insert(i, "v");
    }
    EXPECT_EQ(list.size(), 50);

    for (int i = 0; i < 50; ++i) {
        EXPECT_TRUE(list.remove(i));
    }
    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.begin(), list.end());
}

// =========================================================================
// Large dataset — validate ordering statistically
// =========================================================================

TEST(SkipListTest, LargeInsertAndVerify) {
    IntStringList list;

    constexpr int N = 10000;
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(1, 1000000);

    std::vector<int> inserted;
    inserted.reserve(N);
    for (int i = 0; i < N; ++i) {
        int k = dist(rng);
        inserted.push_back(k);
        list.insert(k, std::to_string(k));
    }

    EXPECT_EQ(list.size(), N);

    // Verify sorted order
    int prev = -1;
    for (auto it = list.begin(); it != list.end(); ++it) {
        EXPECT_GE(it->key, prev);
        prev = it->key;
    }

    // Spot-check: all inserted keys are findable
    std::set<int> missing;
    for (int k : inserted) {
        if (!list.contains(k)) {
            missing.insert(k);
        }
    }
    EXPECT_TRUE(missing.empty()) << missing.size() << " keys not found";

    // find_first_gte for known values
    for (int k : inserted) {
        const auto* n = list.find_first_gte(k);
        ASSERT_NE(n, nullptr);
        EXPECT_EQ(n->key, k);
    }
}

// =========================================================================
// Memory estimation
// =========================================================================

TEST(SkipListTest, MemoryUsageIncreases) {
    IntStringList list;
    EXPECT_GT(list.memory_usage(), 0); // head node

    size_t prev_mem = list.memory_usage();
    for (int i = 0; i < 100; ++i) {
        list.insert(i, "some_value");
        EXPECT_GT(list.memory_usage(), prev_mem);
        prev_mem = list.memory_usage();
    }
}

TEST(SkipListTest, MemoryUsageAfterRemove) {
    IntStringList list;
    for (int i = 0; i < 100; ++i) {
        list.insert(i, "val");
    }
    size_t mem_before = list.memory_usage();

    list.remove(50);
    EXPECT_LT(list.memory_usage(), mem_before);
}

// =========================================================================
// Concurrency: 1 writer + N readers
// =========================================================================

TEST(SkipListTest, ConcurrentSingleWriterMultiReader) {
    IntStringList list;

    // Pre-populate
    for (int i = 0; i < 500; ++i) {
        list.insert(i, "pre");
    }

    std::atomic<bool> stop{false};
    std::atomic<int> read_errors{0};
    std::atomic<int> reads_completed{0};

    // Spawn N reader threads
    constexpr int kReaders = 4;
    std::vector<std::thread> readers;
    readers.reserve(kReaders);
    for (int t = 0; t < kReaders; ++t) {
        readers.emplace_back([&list, &stop, &read_errors, &reads_completed]() {
            while (!stop.load(std::memory_order_relaxed)) {
                // Random reads
                for (int k = 0; k < 500; ++k) {
                    const auto* n = list.find_first_gte(k);
                    if (n != nullptr && n->key < k) {
                        read_errors.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                // Random iteration
                int count = 0;
                for (auto it = list.begin(); it != list.end(); ++it) {
                    ++count;
                    if (count > 100000)
                        break; // safety
                }
                reads_completed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Writer thread inserts continuously
    std::thread writer([&list, &stop]() {
        int base = 500;
        for (int round = 0; round < 50 && !stop.load(std::memory_order_relaxed); ++round) {
            for (int i = 0; i < 100; ++i) {
                list.insert(base + round * 100 + i, "new");
                // Small sleep to interleave with readers
                std::this_thread::yield();
            }
        }
    });

    writer.join();
    stop.store(true, std::memory_order_relaxed);

    for (auto& t : readers) {
        t.join();
    }

    EXPECT_EQ(read_errors.load(), 0) << "Readers saw inconsistent state";
    EXPECT_GT(reads_completed.load(), 0) << "No reads completed";
    EXPECT_GE(list.size(), 500);
}

// =========================================================================
// String keys
// =========================================================================

TEST(SkipListTest, StringKeys) {
    SkipList<std::string, int, StringCmp> list;

    list.insert("banana", 2);
    list.insert("apple", 1);
    list.insert("cherry", 3);
    list.insert("apple", 99); // duplicate

    EXPECT_EQ(list.size(), 4);

    const auto* n = list.find_first_gte("apple");
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->key, "apple");
    EXPECT_EQ(n->value, 1); // first insertion

    n = list.find_first_gte("blueberry");
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->key, "cherry"); // first >= blueberry

    // Ordered iteration
    std::vector<std::string> order;
    for (const auto& node : list) {
        order.push_back(node.key);
    }
    ASSERT_EQ(order.size(), 4);
    EXPECT_EQ(order[0], "apple");
    EXPECT_EQ(order[1], "apple");
    EXPECT_EQ(order[2], "banana");
    EXPECT_EQ(order[3], "cherry");
}

} // namespace
} // namespace pl
