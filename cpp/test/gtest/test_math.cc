#include <gtest/gtest.h>

#include "math.h"

TEST(MathTest, add)
{
  EXPECT_EQ(3, test_gtest::Math<uint32_t>::add(1, 2));
}
