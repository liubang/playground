//=====================================================================
//
// test_log.cpp -
//
// Created by liubang on 2023/06/14 00:10
// Last Modified: 2023/06/14 00:10
//
//=====================================================================
#include <gtest/gtest.h>

#include "absl/log/check.h"
#include "absl/log/log.h"

TEST(abseil, log) {
  // absl::ParseCommandLine(argc, argv);
  // absl::InitializeLog();
  LOG(INFO) << "Hello world!";
  CHECK(1 != 2) << "oops!";
}
