//=====================================================================
//
// traits_test.cpp -
//
// Created by liubang on 2023/05/21 01:33
// Last Modified: 2023/05/21 01:33
//
//=====================================================================
#include "cpp/meta/traits.h"

#include <gtest/gtest.h>

#include <cassert>
#include <string>

#include "cpp/meta/ut/traits_test.h"

namespace {
struct Foo {
  void Init() {}
};
}  // namespace

TEST(meta, traits) {
  static_assert(pl::is_floating_point<float>::value);
  static_assert(pl::is_floating_point<double>::value);
  static_assert(pl::is_floating_point<long double>::value);
  static_assert(!pl::is_floating_point<int>::value);

  static_assert(pl::is_same<int, int>::value);
  static_assert(pl::is_same<std::string, std::string>::value);
  static_assert(!pl::is_same<int, long>::value);

  static_assert(
      pl::is_same<int, pl::remove_const_t<const int>>::value);

  static_assert(
      pl::is_same<
          int, pl::conditional<true, int, std::string>::type>::value);

  static_assert(pl::is_same<
                std::string,
                pl::conditional<false, int, std::string>::type>::value);

  static_assert(
      pl::is_same_v<int, pl::array_size<int[5]>::value_type>);

  static_assert(pl::array_size<int[5]>::value == 5);

  static_assert(pl::ut::numEq(3, 3));

  assert(pl::ut::numEq(3.1, 3.1));

  assert(pl::ut::numEqNew(3, 3));
  assert(pl::ut::numEqNew(3.1, 3.1));

  assert(!pl::HasTypeMember<int>::value);
  assert(pl::HasTypeMember<std::true_type>::value);

  assert(!pl::HasInit<int>::value);
  assert(pl::HasInit<Foo>::value);
}
