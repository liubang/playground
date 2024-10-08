// Copyright (c) 2024 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)

#include "cpp/pl/status/status.h"
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

    auto invalid_argument = pl::Status::NewInvalidArgument("xxx");
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
