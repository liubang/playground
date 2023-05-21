//=====================================================================
//
// tuple_iteration_apply_test.cpp -
//
// Created by liubang on 2023/05/21 23:23
// Last Modified: 2023/05/21 23:23
//
//=====================================================================
#include <tuple>
#include <utility>

#include <gtest/gtest.h>

#include "cpp/meta/tuple_iteration_apply.h"

TEST(meta, PrintTupleApplyFn) {
  std::tuple tp{10, 20, "hello"};
  playground::cpp::meta::PrintTupleApplyFn(tp);
  std::cout << std::endl;
  EXPECT_TRUE(true);
}

TEST(meta, PrintTupleApply) {
  std::tuple tp{10, 20, 3.14, 42, "hello"};
  playground::cpp::meta::PrintTupleApply(tp);
  std::cout << std::endl;
  EXPECT_TRUE(true);
}
