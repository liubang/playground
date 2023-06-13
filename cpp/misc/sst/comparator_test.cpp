//=====================================================================
//
// comparator_test.cpp -
//
// Created by liubang on 2023/05/31 14:32
// Last Modified: 2023/05/31 14:32
//
//=====================================================================

#include "cpp/misc/sst/comparator.h"

#include <gtest/gtest.h>
#include <memory>

TEST(comparator, compara) {
  auto* comparator = pl::misc::sst::bytewiseComparator();
  std::unique_ptr<pl::misc::sst::Comparator> comparator_ptr(
      comparator);

  pl::tools::Binary b1 = "abc";
  pl::tools::Binary b2 = "abd";
  pl::tools::Binary b3 = "abc";
  pl::tools::Binary b4 = "aba";
  pl::tools::Binary b5 = "";
  pl::tools::Binary b6 = "abcccc";

  EXPECT_TRUE(comparator_ptr->compare(b1, b2) < 0);
  EXPECT_TRUE(comparator_ptr->compare(b1, b3) == 0);
  EXPECT_TRUE(comparator_ptr->compare(b1, b4) > 0);

  EXPECT_TRUE(comparator_ptr->compare(b1, b5) > 0);
  EXPECT_TRUE(comparator_ptr->compare(b1, b6) < 0);
}
