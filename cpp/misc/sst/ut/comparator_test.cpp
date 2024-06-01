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

#include "cpp/misc/sst/comparator.h"

#include <gtest/gtest.h>
#include <memory>

namespace pl {

class ComparatorTest : public ::testing::Test {

    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(ComparatorTest, compara) {
    auto comparator = std::make_unique<BytewiseComparator>();

    pl::Binary b1 = "abc";
    pl::Binary b2 = "abd";
    pl::Binary b3 = "abc";
    pl::Binary b4 = "aba";
    pl::Binary b5 = "";
    pl::Binary b6 = "abcccc";

    EXPECT_TRUE(comparator->compare(b1, b2) < 0);
    EXPECT_TRUE(comparator->compare(b1, b3) == 0);
    EXPECT_TRUE(comparator->compare(b1, b4) > 0);
    EXPECT_TRUE(comparator->compare(b1, b5) > 0);
    EXPECT_TRUE(comparator->compare(b1, b6) < 0);
}

} // namespace pl
