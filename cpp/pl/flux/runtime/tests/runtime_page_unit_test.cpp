// Copyright (c) 2026 The Authors. All rights reserved.
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
// Created: 2026/05/15 00:31

#include "cpp/pl/flux/runtime/runtime_page.h"
#include "gtest/gtest.h"
#include <memory>
#include <utility>
#include <vector>

namespace pl::flux {
namespace {

std::shared_ptr<ObjectValue> row(std::vector<std::pair<std::string, Value>> props) {
    return std::make_shared<ObjectValue>(std::move(props));
}

TEST(RuntimePageTest, BuildsColumnarPageFromRows) {
    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.push_back(row({{"host", Value::string("edge-1")}, {"usage", Value::floating(71.5)}}));
    rows.push_back(row({{"host", Value::string("edge-2")}, {"usage", Value::floating(88.0)}}));

    Page page = PageFromRows("cpu", std::move(rows));

    EXPECT_EQ("cpu", page.bucket);
    ASSERT_EQ(1, page.chunks.size());
    EXPECT_EQ(2, page.chunks[0].row_count);
    ASSERT_EQ(2, page.chunks[0].columns.size());
    EXPECT_EQ("host", page.chunks[0].columns[0].name);
    EXPECT_EQ(Value::Type::String, page.chunks[0].columns[0].type);
    EXPECT_EQ("\"edge-2\"", page.chunks[0].columns[0].values[1].string());
    EXPECT_EQ("usage", page.chunks[0].columns[1].name);
    EXPECT_EQ(Value::Type::Float, page.chunks[0].columns[1].type);
    EXPECT_EQ("88", page.chunks[0].columns[1].values[1].string());
}

TEST(RuntimePageTest, MaterializesPageAtBoundary) {
    TableChunk chunk;
    chunk.columns = {"host", "usage"};
    chunk.rows.push_back(row({{"host", Value::string("edge-1")}, {"usage", Value::integer(70)}}));
    chunk.rows.push_back(row({{"host", Value::string("edge-2")}}));

    Page page = PageFromTableChunks("cpu", {chunk});
    TableValue table = TableValueFromPage(page);

    EXPECT_EQ("cpu", table.bucket);
    ASSERT_EQ(2, table.rows.size());
    EXPECT_EQ("\"edge-1\"", table.rows[0]->lookup("host")->string());
    EXPECT_EQ("70", table.rows[0]->lookup("usage")->string());
    EXPECT_TRUE(table.rows[1]->lookup("usage")->is_null());
}

TEST(RuntimePageTest, PreservesEmptyPageAsPageMetadata) {
    Page page = PageFromRows("empty", {});

    EXPECT_EQ("empty", page.bucket);
    ASSERT_EQ(1, page.chunks.size());
    EXPECT_EQ(0, page.chunks[0].row_count);
    EXPECT_TRUE(page.empty());
    TableValue table = TableValueFromPage(page);
    EXPECT_TRUE(table.rows.empty());
}

} // namespace
} // namespace pl::flux
