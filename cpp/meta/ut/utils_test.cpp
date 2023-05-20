//=====================================================================
//
// utils_test.cpp -
//
// Created by liubang on 2023/05/21 01:33
// Last Modified: 2023/05/21 01:33
//
//=====================================================================
#include "cpp/meta/utils.h"

#include <gtest/gtest.h>

#include "cpp/meta/traits.h"

TEST(meta, utils) {
  using arr345 = std::array<std::array<std::array<int, 3>, 4>, 5>;
  // 这里的纬度是逆序的
  static_assert(playground::cpp::meta::is_same_v<
                arr345, playground::cpp::meta::Array<int, 5, 4, 3>::type>);
  playground::cpp::meta::print(1, 2, 3, 4, 5);
}
