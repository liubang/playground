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
        const Value& /*value*/, bool /*normalize_time*/) const override {
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
                {.value = Value::integer(17)},
                {.value = Value::integer(29)},
            },
    };
    ScanRequest request;
    request.predicates.push_back({
        .op = PredicateOp::Eq,
        .column = "host",
        .literal = Value::string("edge-1"),
    });
    request.limit = 3;
    const TableSchema schema{
        .columns =
            {
                {.name = "host", .type = Value::Type::String},
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

} // namespace
} // namespace pl::flux::connector
