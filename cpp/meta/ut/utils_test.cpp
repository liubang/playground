//=====================================================================
//
// utils_test.cpp -
//
// Created by liubang on 2023/05/21 01:33
// Last Modified: 2023/05/21 01:33
//
//=====================================================================
#include "cpp/meta/utils.h"
#include "cpp/meta/traits.h"
#include "cpp/meta/ut/utils_test.h"

#include <gtest/gtest.h>

TEST(meta, array) {
  using arr345 = std::array<std::array<std::array<int, 3>, 4>, 5>;
  // 这里的纬度是逆序的
  static_assert(
      playground::cpp::meta::is_same_v<arr345, playground::cpp::meta::Array<int, 5, 4, 3>::type>);
  playground::cpp::meta::print(1, 2, 3, 4, 5);
  ASSERT_TRUE(true);
}

TEST(meta, map) {
  using longList = playground::cpp::meta::TypeList<char, float, int, double, char>;
  static_assert(
      std::is_same_v<playground::cpp::meta::Map<longList, std::add_pointer>::type,
                     playground::cpp::meta::TypeList<char*, float*, int*, double*, char*>>);
  ASSERT_TRUE(true);
}

TEST(meta, filter) {
  using longList = playground::cpp::meta::TypeList<char, float, int, double, char>;

  static_assert(std::is_same_v<
                playground::cpp::meta::Filter<longList, playground::cpp::meta::sizeLess4>::type,
                playground::cpp::meta::TypeList<char, char>>);
}
