//=====================================================================
//
// fs_test.cpp -
//
// Created by liubang on 2023/05/29 16:19
// Last Modified: 2023/05/29 16:19
//
//=====================================================================

#include "cpp/misc/fs/fs.h"

#include <gtest/gtest.h>

TEST(FsWriter, append) {
  auto *fs = playground::cpp::misc::fs::Fs::getInstance();
  playground::cpp::misc::fs::FsWriter *fw;
  fs->newFsWriter("/tmp/aaa", &fw);

  playground::cpp::tools::Binary data("hello world");
  fw->append(data);
  fw->flush();
  fw->close();
  EXPECT_TRUE(true);
}
