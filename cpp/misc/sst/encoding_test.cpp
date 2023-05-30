//=====================================================================
//
// encoding_test.cpp -
//
// Created by liubang on 2023/05/30 10:21
// Last Modified: 2023/05/30 10:21
//
//=====================================================================

#include "cpp/misc/sst/encoding.h"

#include <gtest/gtest.h>

using playground::cpp::tools::Binary;

TEST(encoding, putAndGetInt) {
  std::string dst;
  int32_t a = 12345;
  playground::cpp::misc::sst::putInt(&dst, a);
  Binary binary(dst);

  int32_t aa;
  playground::cpp::misc::sst::getInt(&binary, &aa);
  EXPECT_EQ(a, aa);

  int64_t b = 123456789;
  dst.clear();
  playground::cpp::misc::sst::putInt(&dst, b);
  binary.reset(dst);

  int64_t bb;
  playground::cpp::misc::sst::getInt(&binary, &bb);
  EXPECT_EQ(b, bb);

  int8_t c = 2;
  dst.clear();
  playground::cpp::misc::sst::putInt(&dst, c);
  binary.reset(dst);

  int8_t cc;
  playground::cpp::misc::sst::getInt(&binary, &cc);
  EXPECT_EQ(c, cc);
}
