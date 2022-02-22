#include <gtest/gtest.h>

#include <tuple>
#include <utility>

#include "tuple_iteration_apply.h"

TEST(cppstories, PrintTupleApplyFn) {
  std::tuple tp{10, 20, "hello"};
  highkyck::cppstories::PrintTupleApplyFn(tp);
  std::cout << std::endl;
  EXPECT_TRUE(true);
}
