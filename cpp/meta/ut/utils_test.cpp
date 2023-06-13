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
#include "cpp/meta/ut/utils_test.h"

TEST(utils, array) {
  using arr345 = std::array<std::array<std::array<int, 3>, 4>, 5>;
  // 这里的纬度是逆序的
  static_assert(
      pl::meta::is_same_v<arr345, pl::meta::Array<int, 5, 4, 3>::type>);
  pl::meta::print(1, 2, 3, 4, 5);
  ASSERT_TRUE(true);
}

TEST(utils, map) {
  using longList = pl::meta::TypeList<char, float, int, double, char>;
  static_assert(
      std::is_same_v<pl::meta::Map<longList, std::add_pointer>::type,
                     pl::meta::TypeList<char*, float*, int*, double*, char*>>);
  ASSERT_TRUE(true);
}

TEST(utils, filter) {
  using longList = pl::meta::TypeList<char, float, int, double, char>;

  static_assert(
      std::is_same_v<pl::meta::Filter<longList, pl::meta::sizeLess4>::type,
                     pl::meta::TypeList<char, char>>);
}

TEST(utils, fold) {
  using longList = pl::meta::TypeList<char, float, int, double, char>;
  static_assert(pl::meta::Fold<longList, std::integral_constant<size_t, 0>,
                               pl::meta::TypeSizeAcc>::type::value == 18);
}

TEST(utils, concat) {
  static_assert(
      std::is_same_v<pl::meta::Concat_t<pl::meta::TypeList<int, double>,
                                        pl::meta::TypeList<char, float>>,
                     pl::meta::TypeList<int, double, char, float>>);
}

TEST(utils, elem) {
  using longList = pl::meta::TypeList<char, float, int, double, char>;
  static_assert(pl::meta::Elem<longList, char>::value);
  static_assert(!pl::meta::Elem<longList, std::string>::value);
}

TEST(utils, unique) {
  using longList = pl::meta::TypeList<char, float, int, double, char>;
  static_assert(
      std::is_same_v<pl::meta::Unique<longList>::type,
                     pl::meta::TypeList<char, float, int, double>::type>);
}
