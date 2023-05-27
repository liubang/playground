//=====================================================================
//
// murmurhash2_test.cc -
//
// Created by liubang on 2023/05/21 22:42
// Last Modified: 2023/05/21 22:42
//
//=====================================================================
#include <gtest/gtest.h>

#include "cpp/misc/hash/murmurhash2.h"

TEST(hash, murmurhash2_test) {
  const char* data = "hello world";
  uint64_t seed = 0;
  playground::cpp::misc::hash::CMurmurHash64 hasher;
  for (uint32_t i = 0; i < 4; ++i) {
    hasher.begin(seed);
    hasher.add(data, strlen(data), false);
    seed = hasher.end();
  }

  EXPECT_NE(0, seed);
}
