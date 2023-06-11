//=====================================================================
//
// bloom_filter_test.cpp -
//
// Created by liubang on 2023/05/21 22:51
// Last Modified: 2023/05/21 22:51
//
//=====================================================================
#include "cpp/misc/bloom/bloom_filter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "cpp/tools/random.h"
#include "cpp/tools/scope.h"

TEST(bloom, bloom) {
  playground::cpp::misc::bloom::BloomFilter filter(32);

  auto* binaries = new playground::cpp::tools::Binary[100];

  SCOPE_EXIT { delete[] binaries; };

  std::vector<std::string> strs(100);

  for (int i = 0; i < 100; ++i) {
    strs[i] = playground::cpp::tools::random_string(i + 10);
    binaries[i] = playground::cpp::tools::Binary(strs[i]);
  }

  std::string dst;
  filter.create(binaries, 100, &dst);

  playground::cpp::tools::Binary ff(dst);

  for (int i = 0; i < 100; ++i) {
    EXPECT_TRUE(filter.contains(binaries[i], ff));
    auto str = playground::cpp::tools::random_string(128);
    EXPECT_FALSE(filter.contains(str, ff));
  }
}
