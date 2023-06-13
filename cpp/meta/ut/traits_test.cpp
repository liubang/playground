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
  static_assert(pl::meta::is_floating_point<float>::value);
  static_assert(pl::meta::is_floating_point<double>::value);
  static_assert(pl::meta::is_floating_point<long double>::value);
  static_assert(!pl::meta::is_floating_point<int>::value);

  static_assert(pl::meta::is_same<int, int>::value);
  static_assert(pl::meta::is_same<std::string, std::string>::value);
  static_assert(!pl::meta::is_same<int, long>::value);

  static_assert(
      pl::meta::is_same<int, pl::meta::remove_const_t<const int>>::value);

  static_assert(
      pl::meta::is_same<
          int, pl::meta::conditional<true, int, std::string>::type>::value);

  static_assert(pl::meta::is_same<
                std::string,
                pl::meta::conditional<false, int, std::string>::type>::value);

  static_assert(
      pl::meta::is_same_v<int, pl::meta::array_size<int[5]>::value_type>);

  static_assert(pl::meta::array_size<int[5]>::value == 5);

  static_assert(pl::meta::ut::numEq(3, 3));

  assert(pl::meta::ut::numEq(3.1, 3.1));

  assert(pl::meta::ut::numEqNew(3, 3));
  assert(pl::meta::ut::numEqNew(3.1, 3.1));

  assert(!pl::meta::HasTypeMember<int>::value);
  assert(pl::meta::HasTypeMember<std::true_type>::value);

  assert(!pl::meta::HasInit<int>::value);
  assert(pl::meta::HasInit<Foo>::value);
}
