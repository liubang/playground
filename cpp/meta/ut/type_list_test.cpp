//=====================================================================
//
// type_list_test.cpp -
//
// Created by liubang on 2023/06/02 15:35
// Last Modified: 2023/06/02 15:35
//
//=====================================================================

#include "cpp/meta/type_list.h"

#include <gtest/gtest.h>

#include <tuple>
#include <type_traits>
#include <variant>

TEST(meta, type_list) {
  using AList = pl::meta::TypeList<int, char>;
  static_assert(pl::meta::TL<AList>);
  static_assert(AList::size == 2);
  static_assert(std::is_same_v<AList::prepend<double>,
                               pl::meta::TypeList<double, int, char>>);

  static_assert(std::is_same_v<AList::append<double>,
                               pl::meta::TypeList<int, char, double>>);

  static_assert(std::is_same_v<AList::to<std::tuple>, std::tuple<int, char>>);
  static_assert(
      std::is_same_v<AList::to<std::variant>, std::variant<int, char>>);
}