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
  std::unique_ptr<playground::cpp::misc::sst::FilterPolicy> filter_policy_ptr(
      playground::cpp::misc::sst::newBloomFilterPolicy(64));
  playground::cpp::misc::sst::FilterBlockBuilder builder(filter_policy_ptr.get());

  std::vector<std::string> keys;

  builder.startBlock(0);
  for (int i = 0; i < 1000; ++i) {
    auto str = playground::cpp::tools::random_string(16);
    builder.addKey(str);
    keys.push_back(str);
  }

  auto filter = builder.finish();
  playground::cpp::misc::sst::FilterBlockReader reader(filter_policy_ptr.get(), filter);

  for (int i = 0; i < 1000; ++i) {
    auto ret = reader.keyMayMatch(0, keys[i]);
    EXPECT_TRUE(ret);
  }
}
