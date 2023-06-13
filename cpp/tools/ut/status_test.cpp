//=====================================================================
//
// status_test.cpp -
//
// Created by liubang on 2023/05/26 17:32
// Last Modified: 2023/05/26 17:32
//
//=====================================================================

#include "cpp/tools/status.h"
#include <gtest/gtest.h>

TEST(Status, constructor) {
  auto ok = pl::Status::NewOk("ok");
  EXPECT_TRUE(ok.isOk());
  EXPECT_FALSE(ok.isNotFound());
  EXPECT_FALSE(ok.isCorruption());
  EXPECT_FALSE(ok.isNotSupported());
  EXPECT_FALSE(ok.isInvalidArgument());
  EXPECT_FALSE(ok.isIOError());

  auto not_found = pl::Status::NewNotFound("xxx");
  EXPECT_TRUE(not_found.isNotFound());
  EXPECT_FALSE(not_found.isOk());
  EXPECT_FALSE(not_found.isCorruption());
  EXPECT_FALSE(not_found.isNotSupported());
  EXPECT_FALSE(not_found.isInvalidArgument());
  EXPECT_FALSE(not_found.isIOError());

  auto corruption = pl::Status::NewCorruption("xxx");
  EXPECT_TRUE(corruption.isCorruption());
  EXPECT_FALSE(corruption.isOk());
  EXPECT_FALSE(corruption.isNotFound());
  EXPECT_FALSE(corruption.isNotSupported());
  EXPECT_FALSE(corruption.isInvalidArgument());
  EXPECT_FALSE(corruption.isIOError());

  auto not_supported = pl::Status::NewNotSupported("xxx");
  EXPECT_TRUE(not_supported.isNotSupported());
  EXPECT_FALSE(not_supported.isOk());
  EXPECT_FALSE(not_supported.isNotFound());
  EXPECT_FALSE(not_supported.isCorruption());
  EXPECT_FALSE(not_supported.isInvalidArgument());
  EXPECT_FALSE(not_supported.isIOError());

  auto invalid_argument =
      pl::Status::NewInvalidArgument("xxx");
  EXPECT_TRUE(invalid_argument.isInvalidArgument());
  EXPECT_FALSE(invalid_argument.isOk());
  EXPECT_FALSE(invalid_argument.isNotFound());
  EXPECT_FALSE(invalid_argument.isCorruption());
  EXPECT_FALSE(invalid_argument.isNotSupported());
  EXPECT_FALSE(invalid_argument.isIOError());

  auto io_error = pl::Status::NewIOError("xxxx");
  EXPECT_TRUE(io_error.isIOError());
  EXPECT_FALSE(io_error.isOk());
  EXPECT_FALSE(io_error.isCorruption());
  EXPECT_FALSE(io_error.isNotFound());
  EXPECT_FALSE(io_error.isNotSupported());
  EXPECT_FALSE(io_error.isInvalidArgument());
}

TEST(Status, to_string) {
  auto ok = pl::Status::NewOk("ok");
  EXPECT_EQ("OK", ok.to_string());

  auto not_found = pl::Status::NewNotFound("xxx");
  EXPECT_EQ("NotFound", not_found.to_string());

  auto corruption = pl::Status::NewCorruption("xxx");
  EXPECT_EQ("Corruption", corruption.to_string());

  auto not_supported = pl::Status::NewNotSupported("xxx");
  EXPECT_EQ("NotSupported", not_supported.to_string());

  auto invalid_argument =
      pl::Status::NewInvalidArgument("xxx");
  EXPECT_EQ("InvalidArgument", invalid_argument.to_string());

  auto io_error = pl::Status::NewIOError("xxxx");
  EXPECT_EQ("IOError", io_error.to_string());
}
