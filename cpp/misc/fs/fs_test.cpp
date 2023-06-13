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
#include <memory>

TEST(fs, FsWriterAndFsReader) {
  auto* fs = pl::Fs::getInstance();
  pl::FsWriter* fw;
  pl::FsReader* fr;

  const std::string filename = "./test.log";
  fs->newFsWriter(filename, &fw);
  EXPECT_NE(fw, nullptr);
  std::unique_ptr<pl::FsWriter> fw_ptr(fw);

  fs->newFsReader(filename, &fr);
  EXPECT_NE(fr, nullptr);
  std::unique_ptr<pl::FsReader> fr_ptr(fr);

  pl::Binary data("hello world");
  auto st = fw_ptr->append(data);
  EXPECT_TRUE(st.isOk());
  st = fw_ptr->flush();
  EXPECT_TRUE(st.isOk());
  st = fw_ptr->sync();
  EXPECT_TRUE(st.isOk());
  st = fw_ptr->close();
  EXPECT_TRUE(st.isOk());

  pl::Binary result;
  char buffer[1024];
  auto status = fr_ptr->read(0, 1024, &result, buffer);
  EXPECT_TRUE(status.isOk());
  EXPECT_EQ(std::string("hello world"),
            std::string(result.data(), result.size()));

  ::remove(filename.data());
}
