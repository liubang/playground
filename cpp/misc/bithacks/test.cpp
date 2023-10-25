//=====================================================================
//
// test.cpp -
//
// Created by liubang on 2023/10/25 20:07
// Last Modified: 2023/10/25 20:07
//
//=====================================================================
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
