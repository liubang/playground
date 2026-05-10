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
// Created: 2026/05/09

#include "cpp/pl/flux/connector/mysql_source.h"
#include "gtest/gtest.h"
#include <cstdlib>
#include <optional>

namespace pl::flux::connector {
namespace {

std::optional<std::string> mysql_test_dsn() {
    const char* dsn = std::getenv("FLUX_MYSQL_TEST_DSN");
    if (dsn == nullptr || std::string(dsn).empty()) {
        return std::nullopt;
    }
    return std::string(dsn);
}

TEST(MySQLSourceTest, ParsesMysqlUrlDsn) {
    auto config_or = ParseMySQLDsn("mysql://flux:flux@192.168.50.31:3307/testdb?ssl=true");

    ASSERT_TRUE(config_or.ok()) << config_or.status();
    EXPECT_EQ("192.168.50.31", config_or->host);
    EXPECT_EQ(3307, config_or->port);
    EXPECT_EQ("flux", config_or->user);
    EXPECT_EQ("flux", config_or->password);
    EXPECT_EQ("testdb", config_or->database);
    EXPECT_TRUE(config_or->ssl);
}

TEST(MySQLSourceTest, ParsesTcpDsnWithDefaultPort) {
    auto config_or = ParseMySQLDsn("flux:secret@tcp(localhost)/testdb");

    ASSERT_TRUE(config_or.ok()) << config_or.status();
    EXPECT_EQ("localhost", config_or->host);
    EXPECT_EQ(3306, config_or->port);
    EXPECT_EQ("flux", config_or->user);
    EXPECT_EQ("secret", config_or->password);
    EXPECT_EQ("testdb", config_or->database);
    EXPECT_FALSE(config_or->ssl);
}

TEST(MySQLSourceTest, RejectsInvalidDsnPort) {
    auto config_or = ParseMySQLDsn("flux:secret@tcp(localhost:70000)/testdb");

    ASSERT_FALSE(config_or.ok());
    EXPECT_EQ(absl::StatusCode::kInvalidArgument, config_or.status().code());
}

TEST(MySQLSourceTest, ScansFixtureTableIntoTableValue) {
    auto dsn = mysql_test_dsn();
    if (!dsn.has_value()) {
        GTEST_SKIP() << "set FLUX_MYSQL_TEST_DSN and import "
                        "cpp/pl/flux/examples/cross_source/mysql_metrics.sql to run MySQL "
                        "integration tests";
    }
    MySQLSource source(*dsn, "cpu");

    auto value_or = source.Scan({});

    ASSERT_TRUE(value_or.ok()) << value_or.status();
    ASSERT_EQ(Value::Type::Table, value_or->type());
    const auto& table = value_or->as_table();
    ASSERT_EQ(6, table.rows.size());
    ASSERT_NE(nullptr, table.rows[0]);
    EXPECT_EQ("2024-07-01T10:00:00Z", table.rows[0]->lookup("_time")->string());
    EXPECT_EQ("\"edge-1\"", table.rows[0]->lookup("host")->string());
    EXPECT_EQ("\"west\"", table.rows[0]->lookup("region")->string());
    EXPECT_EQ("71.5", table.rows[0]->lookup("usage")->string());
    EXPECT_EQ("8u", table.rows[0]->lookup("cores")->string());
}

TEST(MySQLSourceTest, ReportsFixtureSchemaColumnNames) {
    auto dsn = mysql_test_dsn();
    if (!dsn.has_value()) {
        GTEST_SKIP() << "set FLUX_MYSQL_TEST_DSN and import "
                        "cpp/pl/flux/examples/cross_source/mysql_metrics.sql to run MySQL "
                        "integration tests";
    }
    MySQLSource source(*dsn, "cpu");

    auto schema_or = source.Schema();

    ASSERT_TRUE(schema_or.ok()) << schema_or.status();
    ASSERT_EQ(7, schema_or->columns.size());
    EXPECT_EQ("_time", schema_or->columns[0].name);
    EXPECT_EQ(Value::Type::Time, schema_or->columns[0].type);
    EXPECT_EQ("host", schema_or->columns[1].name);
    EXPECT_EQ("region", schema_or->columns[2].name);
    EXPECT_EQ("usage", schema_or->columns[3].name);
    EXPECT_EQ("cores", schema_or->columns[4].name);
    EXPECT_EQ(Value::Type::UInt, schema_or->columns[4].type);
    EXPECT_EQ("active", schema_or->columns[5].name);
    EXPECT_EQ(Value::Type::Bool, schema_or->columns[5].type);
    EXPECT_EQ("note", schema_or->columns[6].name);
    EXPECT_TRUE(schema_or->columns[6].nullable);
}

TEST(MySQLSourceTest, PushesDownProjectionTimeRangePredicateSortAndLimit) {
    auto dsn = mysql_test_dsn();
    if (!dsn.has_value()) {
        GTEST_SKIP() << "set FLUX_MYSQL_TEST_DSN and import "
                        "cpp/pl/flux/examples/cross_source/mysql_metrics.sql to run MySQL "
                        "integration tests";
    }
    MySQLSource source(*dsn, "cpu");
    ScanRequest request;
    request.columns = {"host", "usage"};
    request.time_range = TimeRange{
        .start = "2024-07-01T18:00:30+08:00",
        .stop = "2024-07-01T18:04:00+08:00",
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

TEST(MySQLSourceTest, MapsMySQLNativeTypesToFluxValues) {
    auto dsn = mysql_test_dsn();
    if (!dsn.has_value()) {
        GTEST_SKIP() << "set FLUX_MYSQL_TEST_DSN and import "
                        "cpp/pl/flux/examples/cross_source/mysql_metrics.sql to run MySQL "
                        "integration tests";
    }
    MySQLSource source(*dsn, "mysql_types");

    auto schema_or = source.Schema();

    ASSERT_TRUE(schema_or.ok()) << schema_or.status();
    ASSERT_EQ(11, schema_or->columns.size());
    EXPECT_EQ(Value::Type::Bool, schema_or->columns[1].type);
    EXPECT_EQ(Value::Type::Int, schema_or->columns[2].type);
    EXPECT_EQ(Value::Type::UInt, schema_or->columns[3].type);
    EXPECT_EQ(Value::Type::String, schema_or->columns[4].type);
    EXPECT_EQ(Value::Type::String, schema_or->columns[5].type);
    EXPECT_EQ(Value::Type::String, schema_or->columns[6].type);
    EXPECT_EQ(Value::Type::Time, schema_or->columns[7].type);
    EXPECT_EQ(Value::Type::Time, schema_or->columns[8].type);
    EXPECT_TRUE(schema_or->columns[10].nullable);

    auto value_or = source.Scan({});

    ASSERT_TRUE(value_or.ok()) << value_or.status();
    ASSERT_EQ(Value::Type::Table, value_or->type());
    const auto& table = value_or->as_table();
    ASSERT_EQ(1, table.rows.size());
    ASSERT_NE(nullptr, table.rows[0]);
    EXPECT_EQ("true", table.rows[0]->lookup("tiny_bool")->string());
    EXPECT_EQ("-7", table.rows[0]->lookup("tiny_signed")->string());
    EXPECT_EQ("18446744073709551615u", table.rows[0]->lookup("huge_unsigned")->string());
    EXPECT_EQ("\"12345678901234.123456\"", table.rows[0]->lookup("exact_decimal")->string());
    EXPECT_EQ("\"2024-07-02\"", table.rows[0]->lookup("event_date")->string());
    EXPECT_EQ("\"12:34:56.123456\"", table.rows[0]->lookup("event_time")->string());
    EXPECT_EQ("2024-07-02T12:34:56.123456Z", table.rows[0]->lookup("event_datetime")->string());
    EXPECT_EQ("2024-07-02T12:34:56.123456Z", table.rows[0]->lookup("event_timestamp")->string());
    EXPECT_EQ("\"flux\"", table.rows[0]->lookup("payload")->string());
    EXPECT_EQ("null", table.rows[0]->lookup("nullable_text")->string());
}

TEST(MySQLSourceTest, PushesDownDistinctColumn) {
    auto dsn = mysql_test_dsn();
    if (!dsn.has_value()) {
        GTEST_SKIP() << "set FLUX_MYSQL_TEST_DSN and import "
                        "cpp/pl/flux/examples/cross_source/mysql_metrics.sql to run MySQL "
                        "integration tests";
    }
    MySQLSource source(*dsn, "cpu");
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
    ASSERT_EQ(4, table.rows.size());
    ASSERT_NE(nullptr, table.rows[0]);
    EXPECT_EQ("\"edge-1\"", table.rows[0]->lookup("service")->string());
    EXPECT_EQ("\"edge-2\"", table.rows[1]->lookup("service")->string());
    EXPECT_EQ("\"edge-3\"", table.rows[2]->lookup("service")->string());
    EXPECT_EQ("\"edge-4\"", table.rows[3]->lookup("service")->string());
    EXPECT_EQ(nullptr, table.rows[0]->lookup("host"));
}

TEST(MySQLSourceTest, PushesDownGroupedAggregate) {
    auto dsn = mysql_test_dsn();
    if (!dsn.has_value()) {
        GTEST_SKIP() << "set FLUX_MYSQL_TEST_DSN and import "
                        "cpp/pl/flux/examples/cross_source/mysql_metrics.sql to run MySQL "
                        "integration tests";
    }
    MySQLSource source(*dsn, "cpu");
    ScanRequest request;
    request.group_by = {"region"};
    request.aggregate = AggregateRequest{
        .fn = AggregateFunction::Mean,
        .column = "usage",
        .alias = "mean_usage",
    };
    request.order_by.push_back({
        .column = "region",
        .desc = false,
    });

    auto value_or = source.Scan(request);

    ASSERT_TRUE(value_or.ok()) << value_or.status();
    ASSERT_EQ(Value::Type::Table, value_or->type());
    const auto& table = value_or->as_table();
    ASSERT_EQ(2, table.rows.size());
    ASSERT_NE(nullptr, table.rows[0]);
    EXPECT_EQ("\"east\"", table.rows[0]->lookup("region")->string());
    EXPECT_EQ("49.125", table.rows[0]->lookup("mean_usage")->string());
    EXPECT_EQ("\"west\"", table.rows[1]->lookup("region")->string());
    EXPECT_EQ("77.6875", table.rows[1]->lookup("mean_usage")->string());
}

TEST(MySQLSourceTest, RejectsUnknownPushdownColumnAgainstFixtureSchema) {
    auto dsn = mysql_test_dsn();
    if (!dsn.has_value()) {
        GTEST_SKIP() << "set FLUX_MYSQL_TEST_DSN and import "
                        "cpp/pl/flux/examples/cross_source/mysql_metrics.sql to run MySQL "
                        "integration tests";
    }
    MySQLSource source(*dsn, "cpu");
    ScanRequest request;
    request.columns = {"missing"};

    auto value_or = source.Scan(request);

    ASSERT_FALSE(value_or.ok());
    EXPECT_EQ(absl::StatusCode::kInvalidArgument, value_or.status().code());
    EXPECT_NE(std::string::npos, value_or.status().message().find("unknown column"));
    EXPECT_NE(std::string::npos, value_or.status().message().find("available columns"));
}

} // namespace
} // namespace pl::flux::connector
