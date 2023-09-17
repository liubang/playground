//=====================================================================
//
// test_split.cpp -
//
// Created by liubang on 2023/06/13 13:26
// Last Modified: 2023/06/13 13:26
//
//=====================================================================
#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "absl/strings/str_split.h"

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
