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

#include "cpp/pl/sst/cell.h"

#include <gtest/gtest.h>

namespace pl {
class CellTest : public ::testing::Test {
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(CellTest, test) {
    std::string rowkey = "rowkey";
    std::string cf = "cf";
    std::string col = "col";
    std::string val = "this is test cell";
    uint64_t ts = 123456;
    Cell cell(CellType::CT_PUT, rowkey, cf, col, val, ts);
    std::string encoded_cell_key = cell.cellKey().encode();

    CellKey other_ck;
    other_ck.decode(encoded_cell_key, rowkey.size());
    EXPECT_EQ(CellType::CT_PUT, other_ck.cell_type);
    EXPECT_EQ(rowkey, other_ck.rowkey);
    EXPECT_EQ(cf, other_ck.cf);
    EXPECT_EQ(col, other_ck.col);
    EXPECT_EQ(ts, other_ck.timestamp);
}

} // namespace pl
