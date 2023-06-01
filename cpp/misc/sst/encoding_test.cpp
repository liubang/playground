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
#include "cpp/tools/binary.h"

using playground::cpp::tools::Binary;

TEST(encoding, putAndGetInt) {
  std::string dst;
  int32_t a = 12345;
  playground::cpp::misc::sst::encodeInt(&dst, a);
  Binary binary(dst);

  auto aa = playground::cpp::misc::sst::decodeInt<uint32_t>(binary.data());
  EXPECT_EQ(a, aa);

  uint64_t b = 123456789;
  dst.clear();
  playground::cpp::misc::sst::encodeInt(&dst, b);
  binary.reset(dst);

  auto bb = playground::cpp::misc::sst::decodeInt<uint64_t>(binary.data());
  EXPECT_EQ(b, bb);

  uint8_t c = 2;
  dst.clear();
  playground::cpp::misc::sst::encodeInt(&dst, c);
  binary.reset(dst);

  auto cc = playground::cpp::misc::sst::decodeInt<uint8_t>(binary.data());
  EXPECT_EQ(c, cc);
}
