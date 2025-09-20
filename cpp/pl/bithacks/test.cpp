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

#include "all.h"
#include <gtest/gtest.h>

TEST(bithacks, sign_of_integer) {
    EXPECT_EQ(-1, pl::sign_of_integer(-10));
    EXPECT_EQ(0, pl::sign_of_integer(10));
}

TEST(bithacks, opposite_signs) {
    EXPECT_TRUE(pl::opposite_signs(10, -10));
    EXPECT_TRUE(pl::opposite_signs(-10, 10));
    EXPECT_FALSE(pl::opposite_signs(10, 10));
    EXPECT_FALSE(pl::opposite_signs(-10, -10));
    EXPECT_FALSE(pl::opposite_signs(0, 0));
}

TEST(bithacks, abs) {
    EXPECT_EQ(10, pl::abs(10));
    EXPECT_EQ(10, pl::abs(-10));
    EXPECT_EQ(0, pl::abs(0));
}

TEST(bithacks, min) {
    EXPECT_EQ(1, pl::min(1, 2));
    EXPECT_EQ(-1, pl::min(-1, 1));
    EXPECT_EQ(-1, pl::min(0, -1));
    EXPECT_EQ(0, pl::min(0, 1));
}

TEST(bithacks, max) {
    EXPECT_EQ(1, pl::max(1, 0));
    EXPECT_EQ(1, pl::max(1, -1));
    EXPECT_EQ(-1, pl::max(-1, -2));
    EXPECT_EQ(0, pl::max(-1, 0));
}

TEST(bithacks, power_of_2) {
    EXPECT_FALSE(pl::power_of_2(0));
    EXPECT_TRUE(pl::power_of_2(1));
    EXPECT_TRUE(pl::power_of_2(2));
    EXPECT_FALSE(pl::power_of_2(3));
    EXPECT_TRUE(pl::power_of_2(4));
    EXPECT_FALSE(pl::power_of_2(5));
}
