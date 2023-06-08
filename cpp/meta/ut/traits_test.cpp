//=====================================================================
//
// traits_test.cpp -
//
// Created by liubang on 2023/05/21 01:33
// Last Modified: 2023/05/21 01:33
//
//=====================================================================
#include "cpp/meta/traits.h"
#include "cpp/meta/ut/traits_test.h"

#include <assert.h>
#include <string>

#include <gtest/gtest.h>

namespace {
struct Foo {
  void Init() {}
};
}  // namespace

TEST(meta, traits) {
  static_assert(playground::cpp::meta::is_floating_point<float>::value);
  static_assert(playground::cpp::meta::is_floating_point<double>::value);
  static_assert(playground::cpp::meta::is_floating_point<long double>::value);
  static_assert(false == playground::cpp::meta::is_floating_point<int>::value);

  static_assert(playground::cpp::meta::is_same<int, int>::value);
  static_assert(
      playground::cpp::meta::is_same<std::string, std::string>::value);
  static_assert(false == playground::cpp::meta::is_same<int, long>::value);

  static_assert(playground::cpp::meta::is_same<
                int, playground::cpp::meta::remove_const_t<const int>>::value);

  static_assert(
      playground::cpp::meta::is_same<
          int, playground::cpp::meta::conditional<true, int,
                                                  std::string>::type>::value);

  static_assert(playground::cpp::meta::is_same<
                std::string, playground::cpp::meta::conditional<
                                 false, int, std::string>::type>::value);

  static_assert(playground::cpp::meta::is_same_v<
                int, playground::cpp::meta::array_size<int[5]>::value_type>);

  static_assert(playground::cpp::meta::array_size<int[5]>::value == 5);

  static_assert(playground::cpp::meta::ut::numEq(3, 3));

  assert(playground::cpp::meta::ut::numEq(3.1, 3.1));

  assert(playground::cpp::meta::ut::numEqNew(3, 3));
  assert(playground::cpp::meta::ut::numEqNew(3.1, 3.1));

  assert(!playground::cpp::meta::HasTypeMember<int>::value);
  assert(playground::cpp::meta::HasTypeMember<std::true_type>::value);

  assert(!playground::cpp::meta::HasInit<int>::value);
  assert(playground::cpp::meta::HasInit<Foo>::value);
}
