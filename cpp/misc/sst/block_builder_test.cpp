//=====================================================================
//
// block_builder_test.cpp -
//
// Created by liubang on 2023/05/31 14:59
// Last Modified: 2023/05/31 14:59
//
//=====================================================================

#include "cpp/misc/sst/block_builder.h"
#include "cpp/misc/sst/encoding.h"
#include "cpp/tools/random.h"

#include <gtest/gtest.h>
#include <memory>
#include <unordered_map>
#include <vector>

TEST(block_builder, test) {
  auto* comparator = playground::cpp::misc::sst::bytewiseComparator();
  playground::cpp::misc::sst::BlockBuilder block_builder(comparator, 16);
  constexpr int COUNT = 10000;

  EXPECT_TRUE(block_builder.empty());

  std::vector<std::string> keys;
  std::unordered_map<std::string, std::string> kvs;
  const std::string key_prefix = "test_key_";
  for (int i = 0; i < COUNT; ++i) {
    std::string key = key_prefix + std::to_string(i);
    keys.push_back(key);
  }

  std::sort(keys.begin(), keys.end());

  for (int i = 0; i < COUNT; ++i) {
    auto val = playground::cpp::tools::random_string(64);
    block_builder.add(keys[i], val);
    kvs[keys[i]] = val;
  }

  EXPECT_TRUE(!block_builder.empty());

  auto block = block_builder.finish();
  // try to parse
  const char* data = block.data();
  std::size_t size = block.size();
  auto restart_count = playground::cpp::misc::sst::decodeInt<uint32_t>(&data[size - 4]);
  EXPECT_EQ(restart_count, (COUNT / 16) + (COUNT % 16 == 0 ? 0 : 1));

  // parse all restarts
  std::vector<uint32_t> restarts(restart_count + 1);
  for (int i = 0; i < restart_count; ++i) {
    uint32_t offset = 4 * (i + 2);
    auto restart = playground::cpp::misc::sst::decodeInt<uint32_t>(&data[size - offset]);
    restarts[i] = restart;
  }
  // restarts[restart_count] = size - (4 * (restart_count + 1));
  //
  // // parse keys and values
  // for (int i = 0; i < restart_count; ++i) {
  //   uint32_t start = restarts[i];
  //   // 计算每一个restart的长度
  //   uint32_t limit = restarts[i + 1] - start;
  //   // 解析每一个restart中的key和value
  // }

  delete comparator;
}
