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

#include "absl/status/statusor.h"
#include <gtest/gtest.h>

namespace pl {

class TestAbseilStatusOr : public ::testing::Test {
public:
    void SetUp() override {}
    void TearDown() override {}

    absl::StatusOr<int64_t> test1(int a) {
        if (a == 0) {
            return absl::InvalidArgumentError("invalid argument");
        }
        return a;
    }
};

TEST_F(TestAbseilStatusOr, statusor) {
    auto r = test1(0);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ("invalid argument", r.status().message());
    auto rr = test1(12);
    EXPECT_TRUE(rr.ok());
    EXPECT_EQ(12, *rr);
    EXPECT_EQ(12, rr.value());
}

} // namespace pl
