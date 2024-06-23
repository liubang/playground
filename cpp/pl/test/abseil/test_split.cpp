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

#include "absl/strings/str_split.h"

#include <gtest/gtest.h>
#include <string>
#include <string_view>

TEST(abseil, str_split) {
    std::vector<std::string> strs = absl::StrSplit("a,b,c", ",");
    EXPECT_EQ(strs.size(), 3);
    EXPECT_EQ("a", strs[0]);
    EXPECT_EQ("b", strs[1]);
    EXPECT_EQ("c", strs[2]);

    std::vector<std::string_view> strs1 = absl::StrSplit("hello,world,ok", ",");
    EXPECT_EQ(strs1.size(), 3);
    EXPECT_EQ("hello", strs1[0]);
    EXPECT_EQ("world", strs1[1]);
    EXPECT_EQ("ok", strs1[2]);

    std::vector<std::string> strs2 = absl::StrSplit("a,b;c", absl::ByAnyChar(",;"));

    EXPECT_EQ(strs2.size(), 3);
    EXPECT_EQ("a", strs2[0]);
    EXPECT_EQ("b", strs2[1]);
    EXPECT_EQ("c", strs2[2]);
}
