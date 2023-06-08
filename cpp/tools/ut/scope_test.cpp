//=====================================================================
//
// scope_test.cpp -
//
// Created by liubang on 2023/06/08 15:02
// Last Modified: 2023/06/08 15:02
//
//=====================================================================

#include "cpp/tools/scope.h"
#include <gtest/gtest.h>

TEST(tools, scope) {
  static int i = 0;

  {
    ++i;

    SCOPE_EXIT {
      i--;
    };

    EXPECT_EQ(1, i);
  }

  EXPECT_EQ(0, i);
}
