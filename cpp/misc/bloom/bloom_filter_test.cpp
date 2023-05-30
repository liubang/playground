//=====================================================================
//
// bloom_filter_test.cpp -
//
// Created by liubang on 2023/05/21 22:51
// Last Modified: 2023/05/21 22:51
//
//=====================================================================
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cpp/misc/bloom/bloom_filter.h"
#include "cpp/tools/random.h"

TEST(bloom, bloom) {
  playground::cpp::misc::bloom::BloomFilter filter(32);

  std::unique_ptr<playground::cpp::tools::Binary[]> binaries_ptr =
      std::make_unique<playground::cpp::tools::Binary[]>(100);

  auto* binaries = binaries_ptr.get();

  for (int i = 1; i < 100; ++i) {
    auto str = playground::cpp::tools::random_string(i);
    binaries[i] = playground::cpp::tools::Binary(str);
  }

  std::string dst;
  filter.create(binaries, 100, &dst);

  playground::cpp::tools::Binary ff(dst);

  for (int i = 0; i < 100; ++i) {
    auto ret = filter.contains(binaries[i], ff);
    EXPECT_TRUE(ret);
  }
}
