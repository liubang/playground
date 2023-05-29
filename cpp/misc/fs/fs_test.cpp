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
#include <cstdio>

TEST(fs, FsWriterAndFsReader) {
  auto *fs = playground::cpp::misc::fs::Fs::getInstance();
  playground::cpp::misc::fs::FsWriter *fw;
  playground::cpp::misc::fs::FsReader *fr;
  const std::string filename = "./test.log";
  fs->newFsWriter(filename, &fw);
  EXPECT_NE(fw, nullptr);
  fs->newFsReader(filename, &fr);
  EXPECT_NE(fr, nullptr);

  playground::cpp::tools::Binary data("hello world");
  auto st = fw->append(data);
  EXPECT_TRUE(st.isOk());
  st = fw->flush();
  EXPECT_TRUE(st.isOk());
  st = fw->sync();
  EXPECT_TRUE(st.isOk());
  st = fw->close();
  EXPECT_TRUE(st.isOk());

  playground::cpp::tools::Binary result;
  char buffer[1024];
  auto status = fr->read(0, 1024, &result, buffer);
  EXPECT_TRUE(status.isOk());
  EXPECT_EQ(std::string("hello world"), std::string(result.data()));

  ::remove(filename.data());
}
