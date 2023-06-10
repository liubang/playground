//=====================================================================
//
// table_test.cpp -
//
// Created by liubang on 2023/06/04 17:11
// Last Modified: 2023/06/04 17:11
//
//=====================================================================

#include "cpp/misc/sst/table.h"
#include "cpp/misc/fs/fs.h"
#include "cpp/tools/scope.h"

#include <gtest/gtest.h>
#include <iostream>
// #include "absl/cleanup/cleanup.h"

TEST(table, table) {
  auto* options = new playground::cpp::misc::sst::Options();
  playground::cpp::misc::fs::FsReader* reader;
  auto* fs = playground::cpp::misc::fs::Fs::getInstance();
  fs->newFsReader("/tmp/test.sst", &reader);
  playground::cpp::misc::sst::Table* table;

  auto s = playground::cpp::misc::sst::Table::open(options, reader,
                                                   reader->size(), &table);
  EXPECT_TRUE(s.isOk());
  std::cout << s.msg() << std::endl;

  SCOPE_EXIT {
    delete options->comparator;
    delete options->filter_policy;
    delete options;
    delete reader;
    delete table;
  };

  // absl::Cleanup cleanup = [&]() {
  //   delete options->comparator;
  //   delete options->filter_policy;
  //   delete options;
  //   delete reader;
  //   delete table;
  // };

  constexpr int COUNT = 10001;

  for (int i = 0; i < COUNT; ++i) {
    const std::string key_prefix = "test_key_";
    std::string key = key_prefix + std::to_string(i);
    playground::cpp::tools::Binary val;
    auto s = table->get(key, &val);
    EXPECT_TRUE(s.isOk());
  }
}
