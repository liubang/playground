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

#include "cpp/pl/encoding/encoding.h"

#include <gtest/gtest.h>
#include <random>

class EncodingTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// FIX(liubang):
TEST_F(EncodingTest, verify_varint) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint64_t> dis(0, UINT_MAX);

    for (int i = 0; i < 999; ++i) {
        uint64_t number = dis(gen);
        std::string buffer;
        EXPECT_TRUE(pl::varint_encode(number, &buffer));
        uint64_t exp;
        EXPECT_TRUE(pl::varint_decode(buffer, &exp));
        EXPECT_EQ(number, exp);
    }
}
