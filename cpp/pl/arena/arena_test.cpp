// Copyright (c) 2025 The Authors. All rights reserved.
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

#include "cpp/pl/arena/arena.h"
#include <gtest/gtest.h>
#include <utility>

namespace {
struct TestObject {
    int value = 0;
    std::string name;

    TestObject(int v, std::string n) : value(v), name(std::move(n)) {
        std::cout << "TestObject constructed: " << value << ", " << name << '\n';
    }

    ~TestObject() { std::cout << "TestObject destructed: " << value << ", " << name << '\n'; }
};

constexpr std::size_t align_up(std::size_t size, std::size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

} // namespace

TEST(ArenaNew, normal) {
    pl::Arena arena(1024);
    void* raw_ptr = arena.allocate(100);
    ASSERT_NE(nullptr, raw_ptr);
    auto stats = arena.get_stats();
    ASSERT_EQ(1, stats.block_count);
    ASSERT_EQ(1024, stats.total_allocated);
    ASSERT_EQ(align_up(100, alignof(std::max_align_t)), stats.total_used);
    auto* obj = arena.allocate_object<TestObject>(42, "Hell world");
    ASSERT_NE(nullptr, obj);
    stats = arena.get_stats();
    ASSERT_EQ(align_up(100, alignof(std::max_align_t)) + sizeof(TestObject), stats.total_used);
}
