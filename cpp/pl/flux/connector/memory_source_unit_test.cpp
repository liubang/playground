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
// Created: 2026/05/07 00:35

#include "cpp/pl/flux/connector/memory_source.h"
#include "gtest/gtest.h"

namespace pl::flux::connector {
namespace {

std::vector<std::shared_ptr<ObjectValue>> make_rows() {
    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.push_back(std::make_shared<ObjectValue>(std::vector<std::pair<std::string, Value>>{
        {"host", Value::string("edge-1")}, {"usage", Value::floating(91.5)}}));
    return rows;
}

TEST(ArraySourceTest, ScansRowsWithConfiguredBucket) {
    ArraySource source("hosts", make_rows());

    auto value_or = source.Scan({});

    ASSERT_TRUE(value_or.ok()) << value_or.status();
    ASSERT_EQ(Value::Type::Table, value_or->type());
    EXPECT_EQ("hosts", value_or->as_table().bucket);
    ASSERT_EQ(1, value_or->as_table().rows.size());
    EXPECT_EQ("\"edge-1\"", value_or->as_table().rows[0]->lookup("host")->string());
}

TEST(CsvSourceTest, ScansRowsWithCsvBucket) {
    CsvSource source(make_rows());

    auto value_or = source.Scan({});

    ASSERT_TRUE(value_or.ok()) << value_or.status();
    ASSERT_EQ(Value::Type::Table, value_or->type());
    EXPECT_EQ("csv", value_or->as_table().bucket);
    ASSERT_EQ(1, value_or->as_table().rows.size());
    EXPECT_EQ("91.5", value_or->as_table().rows[0]->lookup("usage")->string());
}

TEST(MemorySourceTest, ReportsSchemaFromRows) {
    ArraySource source("hosts", make_rows());

    auto schema_or = source.Schema();

    ASSERT_TRUE(schema_or.ok()) << schema_or.status();
    ASSERT_EQ(2, schema_or->columns.size());
    EXPECT_EQ("host", schema_or->columns[0].name);
    EXPECT_EQ(Value::Type::String, schema_or->columns[0].type);
    EXPECT_EQ("usage", schema_or->columns[1].name);
    EXPECT_EQ(Value::Type::Float, schema_or->columns[1].type);
}

TEST(MemorySourceTest, RejectsPushdownUntilImplemented) {
    ArraySource source("hosts", make_rows());
    ScanRequest request;
    request.limit = 1;

    auto value_or = source.Scan(request);

    ASSERT_FALSE(value_or.ok());
    EXPECT_EQ(absl::StatusCode::kUnimplemented, value_or.status().code());
}

} // namespace
} // namespace pl::flux::connector
