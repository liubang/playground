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

#include "cpp/pl/random/random.h"
#include "cpp/pl/sst/cell.h"

#include <gtest/gtest.h>

namespace pl {
class CellTest : public ::testing::Test {
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(CellTest, cell) {
    for (int i = 0; i < 10; ++i) {
        std::string rowkey = random_string(32);
        std::string cf = random_string(8);
        std::string col = random_string(16);
        std::string val = random_string(16 * 1024);
        uint64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
        CellType ct = (CellType)(i % 4);
        Cell cell(ct, rowkey, cf, col, val, ts);
        std::string encoded_cell_key = cell.cellKey().encode();

        CellKey other_ck;
        other_ck.decode(encoded_cell_key, rowkey.size());
        EXPECT_EQ(ct, other_ck.cell_type);
        EXPECT_EQ(rowkey, other_ck.rowkey);
        EXPECT_EQ(cf, other_ck.cf);
        EXPECT_EQ(col, other_ck.col);
        EXPECT_EQ(ts, other_ck.timestamp);
    }
}

} // namespace pl
