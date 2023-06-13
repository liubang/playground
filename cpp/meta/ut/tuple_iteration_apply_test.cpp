//=====================================================================
//
// tuple_iteration_apply_test.cpp -
//
// Created by liubang on 2023/05/21 23:23
// Last Modified: 2023/05/21 23:23
//
//=====================================================================
#include "cpp/meta/tuple_iteration_apply.h"

#include <gtest/gtest.h>

#include <tuple>
#include <utility>

TEST(meta, PrintTupleApplyFn) {
  std::tuple tp{10, 20, "hello"};
  pl::meta::PrintTupleApplyFn(tp);
  std::cout << std::endl;
  EXPECT_TRUE(true);
}

TEST(meta, PrintTupleApply) {
  std::tuple tp{10, 20, 3.14, 42, "hello"};
  pl::meta::PrintTupleApply(tp);
  std::cout << std::endl;
  EXPECT_TRUE(true);
}
