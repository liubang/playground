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

constexpr const char* kMetricsDb = "cpp/pl/flux/examples/cross_source/metrics.db";

TEST(SQLiteSourceTest, ScansTableIntoTableValue) {
    SQLiteSource source(kMetricsDb, "cpu");

    auto value_or = source.Scan({});

    ASSERT_TRUE(value_or.ok()) << value_or.status();
    ASSERT_EQ(Value::Type::Table, value_or->type());
    const auto& table = value_or->as_table();
    ASSERT_EQ(4, table.rows.size());
    ASSERT_NE(nullptr, table.rows[0]);
    EXPECT_EQ("\"edge-1\"", table.rows[0]->lookup("host")->string());
    EXPECT_EQ("\"west\"", table.rows[0]->lookup("region")->string());
    EXPECT_EQ("71.5", table.rows[0]->lookup("usage")->string());
}

TEST(SQLiteSourceTest, ReportsSchemaColumnNames) {
    SQLiteSource source(kMetricsDb, "cpu");

    auto schema_or = source.Schema();

    ASSERT_TRUE(schema_or.ok()) << schema_or.status();
    ASSERT_EQ(4, schema_or->columns.size());
    EXPECT_EQ("_time", schema_or->columns[0].name);
    EXPECT_EQ("host", schema_or->columns[1].name);
    EXPECT_EQ("region", schema_or->columns[2].name);
    EXPECT_EQ("usage", schema_or->columns[3].name);
}

TEST(SQLiteSourceTest, PushesDownProjectionTimeRangePredicateSortAndLimit) {
    SQLiteSource source(kMetricsDb, "cpu");
    ScanRequest request;
    request.columns = {"host", "usage"};
    request.time_range = TimeRange{
        .start = "2024-07-01T10:00:30Z",
        .stop = "2024-07-01T10:04:00Z",
    };
    request.predicates.push_back({
        .op = PredicateOp::Eq,
        .column = "host",
        .literal = Value::string("edge-1"),
    });
    request.order_by.push_back({
        .column = "usage",
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
    EXPECT_EQ("93.25", table.rows[0]->lookup("usage")->string());
    EXPECT_EQ(nullptr, table.rows[0]->lookup("_time"));
}

TEST(SQLiteSourceTest, PushesDownProjectionAliases) {
    SQLiteSource source(kMetricsDb, "cpu");
    ScanRequest request;
    request.projection_columns.push_back({
        .column = "host",
        .alias = "host",
    });
    request.projection_columns.push_back({
        .column = "usage",
        .alias = "usage",
    });
    request.limit = 1;

    auto value_or = source.Scan(request);

    ASSERT_TRUE(value_or.ok()) << value_or.status();
    ASSERT_EQ(Value::Type::Table, value_or->type());
    const auto& table = value_or->as_table();
    ASSERT_EQ(1, table.rows.size());
    ASSERT_NE(nullptr, table.rows[0]);
    EXPECT_EQ("\"edge-1\"", table.rows[0]->lookup("host")->string());
    EXPECT_EQ("71.5", table.rows[0]->lookup("usage")->string());
    EXPECT_EQ(nullptr, table.rows[0]->lookup("region"));
}

TEST(SQLiteSourceTest, PushesDownDistinctColumn) {
    SQLiteSource source(kMetricsDb, "cpu");
    ScanRequest request;
    request.projection_columns.push_back({
        .column = "host",
        .alias = "service",
    });
    request.distinct = "host";
    request.order_by.push_back({
        .column = "host",
        .desc = false,
    });

    auto value_or = source.Scan(request);

    ASSERT_TRUE(value_or.ok()) << value_or.status();
    ASSERT_EQ(Value::Type::Table, value_or->type());
    const auto& table = value_or->as_table();
    ASSERT_EQ(3, table.rows.size());
    ASSERT_NE(nullptr, table.rows[0]);
    ASSERT_NE(nullptr, table.rows[1]);
    EXPECT_EQ("\"edge-1\"", table.rows[0]->lookup("service")->string());
    EXPECT_EQ("\"edge-2\"", table.rows[1]->lookup("service")->string());
    EXPECT_EQ("\"edge-3\"", table.rows[2]->lookup("service")->string());
    EXPECT_EQ(nullptr, table.rows[0]->lookup("host"));
}

TEST(SQLiteSourceTest, RejectsUnknownPushdownColumn) {
    SQLiteSource source(kMetricsDb, "cpu");
    ScanRequest request;
    request.columns = {"missing"};

    auto value_or = source.Scan(request);

    ASSERT_FALSE(value_or.ok());
    EXPECT_EQ(absl::StatusCode::kInvalidArgument, value_or.status().code());
    EXPECT_NE(std::string::npos, value_or.status().message().find("unknown column"));
}

} // namespace
} // namespace pl::flux::connector
