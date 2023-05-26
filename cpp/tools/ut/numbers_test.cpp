//=====================================================================
//
// numbers_test.cpp -
//
// Created by liubang on 2023/05/21 23:45
// Last Modified: 2023/05/21 23:45
//
//=====================================================================

#include "cpp/tools/numbers.h"
#include <gtest/gtest.h>

TEST(tools, ip) {
  EXPECT_TRUE(playground::cpp::tools::is_power_of_two(1));
  EXPECT_TRUE(playground::cpp::tools::is_power_of_two(4));
  EXPECT_TRUE(playground::cpp::tools::is_power_of_two(8));
  EXPECT_TRUE(playground::cpp::tools::is_power_of_two(16));
  EXPECT_FALSE(playground::cpp::tools::is_power_of_two(10));
  EXPECT_FALSE(playground::cpp::tools::is_power_of_two(12));
}
