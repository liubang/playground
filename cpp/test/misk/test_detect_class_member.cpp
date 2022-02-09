#include <gtest/gtest.h>

#include <cassert>
#include <iostream>

#include "detect_class_member.h"

namespace {
struct Foo {};

struct Bar {
  int x;
};
}  // namespace

TEST(misk, detect_class_member) {
  EXPECT_FALSE(HasX<Foo>::value);
  EXPECT_TRUE(HasX<Bar>::value);
}

