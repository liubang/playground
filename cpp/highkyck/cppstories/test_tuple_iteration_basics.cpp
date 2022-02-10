#include <gtest/gtest.h>

#include <tuple>
#include <utility>

#include "tuple_iteration_basics.h"

TEST(cppstories, PrintTupleManual) {
  std::tuple tp{10, 20, "hello"};
  highkyck::cppstories::PrintTupleManual<decltype(tp), 0, 1, 2>(tp);
  std::cout << std::endl;
  EXPECT_TRUE(true);
}

TEST(cppstories, PrintTupleManualEx) {
  std::tuple tp{10, 20, "hello"};
  highkyck::cppstories::PrintTupleManualEx(tp, std::index_sequence<0, 1, 2>{});
  std::cout << std::endl;
  highkyck::cppstories::PrintTupleManualEx(
      tp, std::make_index_sequence<std::tuple_size_v<decltype(tp)>>{});
  std::cout << std::endl;
  EXPECT_TRUE(true);
}

TEST(cppstories, PrintTupleAutoGetSize) {
  std::tuple tp{10, 20, "hello"};
  highkyck::cppstories::PrintTupleAutoGetSize(tp);
  std::cout << std::endl;
  EXPECT_TRUE(true);
}

TEST(cppstories, PrintTupleFinal) {
  std::tuple tp{10, 20, "hello"};
  highkyck::cppstories::PrintTupleFinal(tp);
  std::cout << std::endl;
  EXPECT_TRUE(true);
}

TEST(cppstories, PrintTupleWithOstream) {
  std::tuple tp{10, 20, "hello"};
  std::cout << tp << std::endl;
  EXPECT_TRUE(true);
}
