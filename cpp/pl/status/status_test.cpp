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
    pl::Status ok(pl::StatusCode::kOK, "OK");
    EXPECT_TRUE(ok);
    EXPECT_TRUE(ok.isOk());
    EXPECT_FALSE(ok.hasPayload());
    EXPECT_EQ(pl::StatusCode::kOK, ok.code());
    EXPECT_EQ("OK", ok.message());
    std::cout << ok.describe() << std::endl;

    pl::Status st(pl::StatusCode::kOK, "OK", "payload");
    EXPECT_TRUE(st);
    EXPECT_TRUE(st.isOk());
    EXPECT_TRUE(st.hasPayload());
    EXPECT_EQ(0, st.payload<std::string>()->compare("payload"));
    EXPECT_EQ(pl::StatusCode::kOK, st.code());
    EXPECT_EQ("OK", st.message());
    std::cout << st.describe() << std::endl;
}
