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
// Created: 2026/05/31 21:44

#include <optional>
#include <string>
#include <utility>

#include "cpp/pl/flux/connector/sql_builder.h"
#include "gtest/gtest.h"

namespace pl::flux::connector {
namespace {

class TestDialect final : public SqlDialect {
public:
    [[nodiscard]] std::string QuoteIdentifier(const std::string& identifier) const override {
        return "\"" + identifier + "\"";
    }

    [[nodiscard]] absl::StatusOr<std::string> FormatLiteral(
        const runtime::Value& /*value*/, bool /*normalize_time*/) const override {
        return absl::UnimplementedError("literal formatting is not used by this test");
    }

    [[nodiscard]] std::string SourceName() const override { return "test"; }

    [[nodiscard]] std::string FormatLimit(std::optional<int64_t> /*limit*/,
                                          std::optional<int64_t> /*offset*/) const override {
        return "";
    }

    [[nodiscard]] std::string UnboundedLimit() const override { return "LIMIT -1"; }
};

TEST(SqlBuilderTest, PreservesBaseParametersBeforeOuterScanParameters) {
    ParameterizedSql base_query{
        .sql = "SELECT * FROM \"cpu\" WHERE rowid >= ? AND rowid <= ?",
        .params =
            {
                {.value = runtime::Value::integer(17)},
                {.value = runtime::Value::integer(29)},
            },
    };
    ScanRequest request;
    request.predicates.push_back({
        .op = PredicateOp::Eq,
        .column = "host",
        .literal = runtime::Value::string("edge-1"),
    });
    request.limit = 3;
    const TableSchema schema{
        .columns =
            {
                {.name = "host", .type = runtime::Value::Type::String},
            },
    };

    auto sql_or = BuildParameterizedScanSql(std::move(base_query), request, schema, TestDialect{});

    ASSERT_TRUE(sql_or.ok()) << sql_or.status();
    EXPECT_EQ("SELECT * FROM (SELECT * FROM \"cpu\" WHERE rowid >= ? AND rowid <= ?) "
              "AS flux_source WHERE \"host\" = ? LIMIT ?",
              sql_or->sql);
    ASSERT_EQ(4, sql_or->params.size());
    EXPECT_EQ("17", sql_or->params[0].value.string());
    EXPECT_EQ("29", sql_or->params[1].value.string());
    EXPECT_EQ("\"edge-1\"", sql_or->params[2].value.string());
    EXPECT_EQ("3", sql_or->params[3].value.string());
}

// Dialect that renders literals inline, for the non-parameterized path.
class LiteralDialect final : public SqlDialect {
public:
    [[nodiscard]] std::string QuoteIdentifier(const std::string& identifier) const override {
        return "\"" + identifier + "\"";
    }

    [[nodiscard]] absl::StatusOr<std::string> FormatLiteral(
        const runtime::Value& value, bool /*normalize_time*/) const override {
        if (value.type() == runtime::Value::Type::String) {
            return "'" + value.as_string() + "'";
        }
        if (value.type() == runtime::Value::Type::Time) {
            return "'" + value.as_time().literal + "'";
        }
        return value.string();
    }

    [[nodiscard]] std::string SourceName() const override { return "test"; }

    [[nodiscard]] std::string FormatLimit(std::optional<int64_t> limit,
                                          std::optional<int64_t> offset) const override {
        std::string out;
        if (limit.has_value()) {
            out += " LIMIT " + std::to_string(*limit);
        }
        if (offset.has_value()) {
            if (!limit.has_value()) {
                out += " LIMIT -1";
            }
            out += " OFFSET " + std::to_string(*offset);
        }
        return out;
    }

    [[nodiscard]] std::string UnboundedLimit() const override { return "LIMIT -1"; }
};

TableSchema CpuSchema() {
    return TableSchema{
        .columns =
            {
                {.name = "_time", .type = runtime::Value::Type::Time},
                {.name = "host", .type = runtime::Value::Type::String},
                {.name = "usage", .type = runtime::Value::Type::Float},
            },
    };
}

TEST(SqlBuilderTest, BuildsTimeRangeAndPredicateWhereClause) {
    ScanRequest request;
    request.time_range = TimeRange{
        .start = "2024-07-01T10:00:30Z",
        .stop = "2024-07-01T10:05:00Z",
    };
    request.predicates.push_back(
        {.op = PredicateOp::Gte, .column = "usage", .literal = runtime::Value::floating(50.0)});
    request.predicates.push_back(
        {.op = PredicateOp::NotEq, .column = "host", .literal = runtime::Value::string("edge-3")});

    auto sql_or = BuildScanSql("SELECT * FROM \"cpu\"", request, CpuSchema(), LiteralDialect{});

    ASSERT_TRUE(sql_or.ok()) << sql_or.status();
    // Time range is start-inclusive and stop-exclusive; predicates follow in
    // declaration order.
    EXPECT_EQ("SELECT * FROM (SELECT * FROM \"cpu\") AS flux_source WHERE \"_time\" >= "
              "'2024-07-01T10:00:30Z' AND \"_time\" < '2024-07-01T10:05:00Z' AND \"usage\" >= "
              "50 AND \"host\" != 'edge-3'",
              *sql_or);
}

TEST(SqlBuilderTest, BuildsGroupedAggregateSelect) {
    ScanRequest request;
    request.group_by = {"host"};
    request.aggregate = AggregateRequest{
        .fn = AggregateFunction::Mean,
        .column = "usage",
        .alias = "avg_usage",
    };

    auto sql_or = BuildScanSql("SELECT * FROM \"cpu\"", request, CpuSchema(), LiteralDialect{});

    ASSERT_TRUE(sql_or.ok()) << sql_or.status();
    EXPECT_EQ("SELECT \"host\", AVG(\"usage\") AS \"avg_usage\" FROM (SELECT * FROM \"cpu\") AS "
              "flux_source GROUP BY \"host\"",
              *sql_or);
}

TEST(SqlBuilderTest, BuildsDistinctGroupBy) {
    ScanRequest request;
    request.distinct = "host";

    auto sql_or = BuildScanSql("SELECT * FROM \"cpu\"", request, CpuSchema(), LiteralDialect{});

    ASSERT_TRUE(sql_or.ok()) << sql_or.status();
    EXPECT_EQ("SELECT \"host\" FROM (SELECT * FROM \"cpu\") AS flux_source GROUP BY \"host\"",
              *sql_or);
}

TEST(SqlBuilderTest, BuildsProjectionAliasOrderByAndLimitOffset) {
    ScanRequest request;
    request.projection_columns = {
        {.column = "host", .alias = ""},
        {.column = "usage", .alias = "cpu_usage"},
    };
    request.order_by = {{.column = "usage", .desc = true}};
    request.limit = 10;
    request.offset = 5;

    auto sql_or = BuildScanSql("SELECT * FROM \"cpu\"", request, CpuSchema(), LiteralDialect{});

    ASSERT_TRUE(sql_or.ok()) << sql_or.status();
    EXPECT_EQ("SELECT \"host\", \"usage\" AS \"cpu_usage\" FROM (SELECT * FROM \"cpu\") AS "
              "flux_source ORDER BY \"usage\" DESC LIMIT 10 OFFSET 5",
              *sql_or);
}

TEST(SqlBuilderTest, ParameterizedWhereBindsLiteralsInOrder) {
    ScanRequest request;
    request.time_range = TimeRange{
        .start = "2024-07-01T10:00:30Z",
        .stop = "2024-07-01T10:05:00Z",
    };
    request.predicates.push_back(
        {.op = PredicateOp::Lt, .column = "usage", .literal = runtime::Value::floating(90.0)});

    auto sql_or =
        BuildParameterizedScanSql("SELECT * FROM \"cpu\"", request, CpuSchema(), TestDialect{});

    ASSERT_TRUE(sql_or.ok()) << sql_or.status();
    EXPECT_EQ("SELECT * FROM (SELECT * FROM \"cpu\") AS flux_source WHERE \"_time\" >= ? AND "
              "\"_time\" < ? AND \"usage\" < ?",
              sql_or->sql);
    ASSERT_EQ(3, sql_or->params.size());
    EXPECT_EQ("\"2024-07-01T10:00:30Z\"", sql_or->params[0].value.string());
    EXPECT_TRUE(sql_or->params[0].normalize_time);
    EXPECT_EQ("\"2024-07-01T10:05:00Z\"", sql_or->params[1].value.string());
    EXPECT_TRUE(sql_or->params[1].normalize_time);
    EXPECT_EQ("90", sql_or->params[2].value.string());
    EXPECT_FALSE(sql_or->params[2].normalize_time);
}

TEST(SqlBuilderTest, ParameterizedOffsetWithoutLimitUsesUnboundedLimit) {
    ScanRequest request;
    request.offset = 20;

    auto sql_or =
        BuildParameterizedScanSql("SELECT * FROM \"cpu\"", request, CpuSchema(), TestDialect{});

    ASSERT_TRUE(sql_or.ok()) << sql_or.status();
    EXPECT_EQ("SELECT * FROM (SELECT * FROM \"cpu\") AS flux_source LIMIT -1 OFFSET ?",
              sql_or->sql);
    ASSERT_EQ(1, sql_or->params.size());
    EXPECT_EQ("20", sql_or->params[0].value.string());
}

TEST(SqlBuilderTest, RejectsUnknownPredicateColumn) {
    ScanRequest request;
    request.predicates.push_back(
        {.op = PredicateOp::Eq, .column = "nope", .literal = runtime::Value::integer(1)});

    auto sql_or = BuildScanSql("SELECT * FROM \"cpu\"", request, CpuSchema(), LiteralDialect{});

    ASSERT_FALSE(sql_or.ok());
    EXPECT_NE(sql_or.status().message().find("unknown column: nope"), std::string::npos);
}

TEST(SqlBuilderTest, RejectsAggregateCombinedWithDistinct) {
    ScanRequest request;
    request.aggregate = AggregateRequest{.fn = AggregateFunction::Count, .column = "usage"};
    request.distinct = "host";

    auto sql_or = BuildScanSql("SELECT * FROM \"cpu\"", request, CpuSchema(), LiteralDialect{});

    ASSERT_FALSE(sql_or.ok());
    EXPECT_NE(sql_or.status().message().find("cannot combine aggregate and distinct"),
              std::string::npos);
}

TEST(SqlBuilderTest, RejectsNegativeLimitAndOffset) {
    ScanRequest limit_request;
    limit_request.limit = -1;
    auto limit_or =
        BuildScanSql("SELECT * FROM \"cpu\"", limit_request, CpuSchema(), LiteralDialect{});
    ASSERT_FALSE(limit_or.ok());
    EXPECT_NE(limit_or.status().message().find("limit must be non-negative"), std::string::npos);

    ScanRequest offset_request;
    offset_request.offset = -3;
    auto offset_or =
        BuildScanSql("SELECT * FROM \"cpu\"", offset_request, CpuSchema(), LiteralDialect{});
    ASSERT_FALSE(offset_or.ok());
    EXPECT_NE(offset_or.status().message().find("offset must be non-negative"), std::string::npos);
}

} // namespace
} // namespace pl::flux::connector
