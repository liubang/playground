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
  static_assert(pl::is_same_v<arr345, pl::Array<int, 5, 4, 3>::type>);
  pl::print(1, 2, 3, 4, 5);
  ASSERT_TRUE(true);
}

TEST(utils, map) {
  using longList = pl::TypeList<char, float, int, double, char>;
  static_assert(
      std::is_same_v<pl::Map<longList, std::add_pointer>::type,
                     pl::TypeList<char*, float*, int*, double*, char*>>);
  ASSERT_TRUE(true);
}

TEST(utils, filter) {
  using longList = pl::TypeList<char, float, int, double, char>;

  static_assert(std::is_same_v<pl::Filter<longList, pl::sizeLess4>::type,
                               pl::TypeList<char, char>>);
}

TEST(utils, fold) {
  using longList = pl::TypeList<char, float, int, double, char>;
  static_assert(pl::Fold<longList, std::integral_constant<size_t, 0>,
                         pl::TypeSizeAcc>::type::value == 18);
}

TEST(utils, concat) {
  static_assert(
      std::is_same_v<
          pl::Concat_t<pl::TypeList<int, double>, pl::TypeList<char, float>>,
          pl::TypeList<int, double, char, float>>);
}

TEST(utils, elem) {
  using longList = pl::TypeList<char, float, int, double, char>;
  static_assert(pl::Elem<longList, char>::value);
  static_assert(!pl::Elem<longList, std::string>::value);
}

TEST(utils, unique) {
  using longList = pl::TypeList<char, float, int, double, char>;
  static_assert(std::is_same_v<pl::Unique<longList>::type,
                               pl::TypeList<char, float, int, double>::type>);
}

TEST(utils, sums) {
  //
  static_assert(pl::sums(1, 2, 3, 4) == 10);
}
