//=====================================================================
//
// filter_block_test.cpp -
//
// Created by liubang on 2023/05/30 15:43
// Last Modified: 2023/05/30 15:43
//
//=====================================================================

#include "cpp/misc/sst/filter_block_builder.h"
#include "cpp/misc/sst/filter_block_reader.h"
#include "cpp/misc/sst/filter_policy.h"
#include "cpp/tools/random.h"

#include <memory>

#include <gtest/gtest.h>

TEST(filter_block, build_and_read) {
  std::unique_ptr<pl::misc::sst::FilterPolicy> filter_policy_ptr(
      pl::misc::sst::newBloomFilterPolicy(64));
  pl::misc::sst::FilterBlockBuilder builder(
      filter_policy_ptr.get());

  std::vector<std::string> keys;

  builder.startBlock(0);
  for (int i = 0; i < 1000; ++i) {
    auto str = pl::tools::random_string(128);
    builder.addKey(str);
    keys.push_back(str);
  }

  auto filter = builder.finish();
  pl::misc::sst::FilterBlockReader reader(filter_policy_ptr.get(),
                                                       filter);

  for (int i = 0; i < 1000; ++i) {
    auto ret = reader.keyMayMatch(0, keys[i]);
    EXPECT_TRUE(ret);
  }

  for (int i = 0; i < 1000; ++i) {
    auto str = pl::tools::random_string(64);
    auto ret = reader.keyMayMatch(0, str);
    EXPECT_FALSE(ret);
  }
}
