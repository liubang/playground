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

std::string random_string(size_t length) {
  auto randchar = []() -> char {
    const char charset[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    const size_t max_index = (sizeof(charset) - 1);
    return charset[rand() % max_index];
  };
  std::string str(length, 0);
  std::generate_n(str.begin(), length, randchar);
  return str;
}

TEST(bloom, bloom) {
  playground::cpp::misc::bloom::BloomFilter filter(static_cast<uint64_t>(16 * 1024 * 1024 * 8));

  const char* data1 = "liubang";
  uint64_t len1 = strlen(data1);
  filter.insert(data1, len1);
  auto res = filter.contains(data1, len1);
  EXPECT_TRUE(res);

  const char* data2 = "other string";
  uint64_t len2 = strlen(data2);
  res = filter.contains(data2, len2);
  EXPECT_FALSE(res);

  std::vector<std::string> strs;
  for (int i = 1; i < 100; ++i) {
    auto str = random_string(i);
    strs.push_back(str);
    filter.insert(str.c_str(), str.size());
  }

  for (auto& str : strs) {
    auto ret = filter.contains(str.data(), str.size());
    EXPECT_TRUE(ret);
  }
}
