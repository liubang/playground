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

#include "cpp/pl/binary/binary.h"

#include <gtest/gtest.h>

TEST(Binary, constructor) {
    pl::Binary b1;
    EXPECT_STREQ(b1.data(), "");
    EXPECT_EQ(0, b1.size());

    pl::Binary b2("hello", 5);
    EXPECT_STREQ(b2.data(), "hello");
    EXPECT_EQ(5, b2.size());

    const std::string s = "hello";
    pl::Binary b3(s);
    EXPECT_STREQ(b3.data(), "hello");
    EXPECT_EQ(5, b3.size());

    const char* cs = "hello";
    pl::Binary b4(cs);
    EXPECT_STREQ(b4.data(), "hello");
    EXPECT_EQ(5, b4.size());

    pl::Binary b5(b4);
    EXPECT_STREQ(b5.data(), "hello");
    EXPECT_EQ(5, b5.size());
}

TEST(Binary, clear) {
    pl::Binary b1("hello", 5);
    EXPECT_STREQ(b1.data(), "hello");
    EXPECT_EQ(5, b1.size());

    b1.clear();
    EXPECT_STREQ(b1.data(), "");
    EXPECT_EQ(0, b1.size());

    EXPECT_TRUE(b1.empty());
}

TEST(Binary, operator) {
    pl::Binary b1("hello", 5);
    EXPECT_EQ('h', b1[0]);
    EXPECT_EQ('e', b1[1]);
    EXPECT_EQ('l', b1[2]);
    EXPECT_EQ('l', b1[3]);
    EXPECT_EQ('o', b1[4]);
}

TEST(Binary, compare) {
    pl::Binary b1("hello", 5);
    pl::Binary b2("helloo", 6);
    pl::Binary b3("hello", 5);

    EXPECT_FALSE(b1 == b2);
    EXPECT_EQ(-1, b1.compare(b2));
    EXPECT_EQ(1, b2.compare(b1));

    EXPECT_TRUE(b1 == b3);
    EXPECT_EQ(0, b1.compare(b3));
    EXPECT_EQ(0, b3.compare(b1));
}
