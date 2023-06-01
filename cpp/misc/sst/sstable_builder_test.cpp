//=====================================================================
//
// sstable_builder_test.cpp -
//
// Created by liubang on 2023/06/01 14:59
// Last Modified: 2023/06/01 14:59
//
//=====================================================================

#include "cpp/misc/sst/sstable_builder.h"
#include "cpp/misc/fs/fs.h"
#include "cpp/tools/random.h"

#include <gtest/gtest.h>
#include <vector>
#include "absl/cleanup/cleanup.h"

TEST(sstable_builder, build) {
  constexpr int COUNT = 10001;
  auto* options = new playground::cpp::misc::sst::Options();
  playground::cpp::misc::fs::FsWriter* writer;
  auto* fs = playground::cpp::misc::fs::Fs::getInstance();
  fs->newFsWriter("./test.sst", &writer);

  auto* sstable_builder = new playground::cpp::misc::sst::SSTableBuilder(options, writer);

  absl::Cleanup cleanup = [&]() {
    delete options->comparator;
    delete options->filter_policy;
    delete options;
    delete writer;
    delete sstable_builder;
  };

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
    sstable_builder->add(keys[i], val);
    kvs[keys[i]] = val;
  }
  sstable_builder->finish();
}
