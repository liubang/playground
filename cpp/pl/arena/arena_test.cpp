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

#include "cpp/pl/arena/arena.h"

#include <gtest/gtest.h>

TEST(arena, allocate) {
    constexpr std::size_t ptr_char_size = sizeof(char*);
    pl::Arena arena;

    (void)arena.allocate(1024);
    int usage = 4096 + ptr_char_size;
    EXPECT_EQ(arena.memory_usage(), usage);

    (void)arena.allocate(2048);
    EXPECT_EQ(arena.memory_usage(), usage);

    (void)arena.allocate(2048);
    usage += 2048 + ptr_char_size;
    EXPECT_EQ(arena.memory_usage(), usage);

    (void)arena.allocate(1);
    EXPECT_EQ(arena.memory_usage(), usage);

    (void)arena.allocate(12345);
    usage += 12345 + ptr_char_size;
    EXPECT_EQ(arena.memory_usage(), usage);
}
