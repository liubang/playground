//=====================================================================
//
// test_log.cpp -
//
// Created by liubang on 2023/06/14 00:10
// Last Modified: 2023/06/14 00:10
//
//=====================================================================
#include "absl/flags/parse.h"
#include "absl/log/check.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"

int main([[maybe_unused]] int argc, [[maybe_unused]] char *argv[]) {
  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();
  LOG(INFO) << "Hello world!";
  CHECK(1 != 2) << "oops!";
  return 0;
}
