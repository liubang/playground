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

TEST(utils, array) {
  using arr345 = std::array<std::array<std::array<int, 3>, 4>, 5>;
  // 这里的纬度是逆序的
  static_assert(
      playground::cpp::meta::is_same_v<arr345, playground::cpp::meta::Array<int, 5, 4, 3>::type>);
  playground::cpp::meta::print(1, 2, 3, 4, 5);
  ASSERT_TRUE(true);
}

TEST(utils, map) {
  using longList = playground::cpp::meta::TypeList<char, float, int, double, char>;
  static_assert(
      std::is_same_v<playground::cpp::meta::Map<longList, std::add_pointer>::type,
                     playground::cpp::meta::TypeList<char*, float*, int*, double*, char*>>);
  ASSERT_TRUE(true);
}

TEST(utils, filter) {
  using longList = playground::cpp::meta::TypeList<char, float, int, double, char>;

  static_assert(std::is_same_v<
                playground::cpp::meta::Filter<longList, playground::cpp::meta::sizeLess4>::type,
                playground::cpp::meta::TypeList<char, char>>);
}

TEST(utils, fold) {
  using longList = playground::cpp::meta::TypeList<char, float, int, double, char>;
  static_assert(playground::cpp::meta::Fold<longList, std::integral_constant<size_t, 0>,
                                            playground::cpp::meta::TypeSizeAcc>::type::value == 18);
}

TEST(utils, concat) {
  static_assert(
      std::is_same_v<playground::cpp::meta::Concat_t<playground::cpp::meta::TypeList<int, double>,
                                                     playground::cpp::meta::TypeList<char, float>>,
                     playground::cpp::meta::TypeList<int, double, char, float>>);
}

TEST(utils, elem) {
  using longList = playground::cpp::meta::TypeList<char, float, int, double, char>;
  static_assert(playground::cpp::meta::Elem<longList, char>::value);
  static_assert(!playground::cpp::meta::Elem<longList, std::string>::value);
}

TEST(utils, unique) {
  using longList = playground::cpp::meta::TypeList<char, float, int, double, char>;
  static_assert(std::is_same_v<playground::cpp::meta::Unique<longList>::type,
                               playground::cpp::meta::TypeList<char, float, int, double>::type>);
}
