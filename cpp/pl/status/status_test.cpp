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
    pl::Status st1(pl::StatusCode::kOK, "OK");
    EXPECT_TRUE(st1);
    EXPECT_TRUE(st1.isOk());
    EXPECT_FALSE(st1.hasPayload());
    EXPECT_EQ(pl::StatusCode::kOK, st1.code());
    EXPECT_EQ("OK", st1.message());
    std::cout << st1.describe() << std::endl;

    pl::Status st2(pl::StatusCode::kOK, "OK");
    st2.setPayload<std::string>("payload");
    EXPECT_TRUE(st2);
    EXPECT_TRUE(st2.isOk());
    EXPECT_TRUE(st2.hasPayload());
    EXPECT_EQ(pl::StatusCode::kOK, st2.code());
    EXPECT_EQ("OK", st2.message());
    std::cout << st2.describe() << std::endl;
    EXPECT_EQ("payload", *st2.payload<std::string>());

    pl::Status st3(pl::StatusCode::kOK, "OK", std::string("payload"));
    EXPECT_TRUE(st3);
    EXPECT_TRUE(st3.isOk());
    EXPECT_TRUE(st3.hasPayload());
    EXPECT_EQ(pl::StatusCode::kOK, st3.code());
    EXPECT_EQ("OK", st3.message());
    std::cout << st3.describe() << std::endl;
    EXPECT_EQ("payload", *st3.payload<std::string>());
}
