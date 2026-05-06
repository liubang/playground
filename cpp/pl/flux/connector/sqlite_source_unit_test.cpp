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

#include "cpp/pl/flux/connector/sqlite_source.h"
#include "gtest/gtest.h"

namespace pl::flux::connector {
namespace {

TEST(SQLiteSourceTest, ScansQueryIntoTableValue) {
    SQLiteSource source(":memory:",
                        "select 1 as count, 2.5 as load, 'edge-1' as host, null as note");

    auto value_or = source.Scan({});

    ASSERT_TRUE(value_or.ok()) << value_or.status();
    ASSERT_EQ(Value::Type::Table, value_or->type());
    const auto& table = value_or->as_table();
    ASSERT_EQ(1, table.rows.size());
    ASSERT_NE(nullptr, table.rows[0]);
    EXPECT_EQ("1", table.rows[0]->lookup("count")->string());
    EXPECT_EQ("2.5", table.rows[0]->lookup("load")->string());
    EXPECT_EQ("\"edge-1\"", table.rows[0]->lookup("host")->string());
    EXPECT_EQ("null", table.rows[0]->lookup("note")->string());
}

TEST(SQLiteSourceTest, ReportsSchemaColumnNames) {
    SQLiteSource source(":memory:", "select 1 as count, 'edge-1' as host");

    auto schema_or = source.Schema();

    ASSERT_TRUE(schema_or.ok()) << schema_or.status();
    ASSERT_EQ(2, schema_or->columns.size());
    EXPECT_EQ("count", schema_or->columns[0].name);
    EXPECT_EQ("host", schema_or->columns[1].name);
}

TEST(SQLiteSourceTest, PushesDownProjectionTimeRangePredicateSortAndLimit) {
    SQLiteSource source(":memory:",
                        "select '2024-01-01T00:00:00Z' as _time, 'edge-2' as host, 20.0 as "
                        "_value union all "
                        "select '2024-01-01T00:01:00Z', 'edge-1', 91.5 union all "
                        "select '2024-01-01T00:02:00Z', 'edge-1', 42.0");
    ScanRequest request;
    request.columns = {"host", "_value"};
    request.time_range = TimeRange{
        .start = "2024-01-01T00:00:30Z",
        .stop = "2024-01-01T00:03:00Z",
    };
    request.predicates.push_back({
        .op = PredicateOp::Eq,
        .column = "host",
        .literal = Value::string("edge-1"),
    });
    request.order_by.push_back({
        .column = "_value",
        .desc = true,
    });
    request.limit = 1;

    auto value_or = source.Scan(request);

    ASSERT_TRUE(value_or.ok()) << value_or.status();
    ASSERT_EQ(Value::Type::Table, value_or->type());
    const auto& table = value_or->as_table();
    ASSERT_EQ(1, table.rows.size());
    ASSERT_NE(nullptr, table.rows[0]);
    EXPECT_EQ("\"edge-1\"", table.rows[0]->lookup("host")->string());
    EXPECT_EQ("91.5", table.rows[0]->lookup("_value")->string());
    EXPECT_EQ(nullptr, table.rows[0]->lookup("_time"));
}

TEST(SQLiteSourceTest, RejectsUnknownPushdownColumn) {
    SQLiteSource source(":memory:", "select 1 as count");
    ScanRequest request;
    request.columns = {"missing"};

    auto value_or = source.Scan(request);

    ASSERT_FALSE(value_or.ok());
    EXPECT_EQ(absl::StatusCode::kInvalidArgument, value_or.status().code());
    EXPECT_NE(std::string::npos, value_or.status().message().find("unknown column"));
}

} // namespace
} // namespace pl::flux::connector
