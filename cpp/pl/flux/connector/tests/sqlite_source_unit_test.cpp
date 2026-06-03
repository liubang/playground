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

#include <memory>
#include <sqlite3.h>
#include <string>

#include "cpp/pl/flux/connector/sqlite_source.h"
#include "gtest/gtest.h"

namespace pl::flux::connector {
namespace {

constexpr const char* kMetricsDb = "cpp/pl/flux/examples/cross_source/metrics.db";

struct SqliteTestDbDeleter {
    void operator()(sqlite3* db) const {
        if (db != nullptr) {
            sqlite3_close(db);
        }
    }
};

using SqliteTestDb = std::unique_ptr<sqlite3, SqliteTestDbDeleter>;

void ExecuteSql(sqlite3* db, const char* sql) {
    char* error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    std::unique_ptr<char, decltype(&sqlite3_free)> error_guard(error, sqlite3_free);
    ASSERT_EQ(SQLITE_OK, rc) << (error == nullptr ? "" : error);
}

void CreateBoolMetricsDb(const std::string& path) {
    sqlite3* raw_db = nullptr;
    ASSERT_EQ(SQLITE_OK,
              sqlite3_open_v2(
                  path.c_str(), &raw_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr));
    SqliteTestDb db(raw_db);
    ExecuteSql(db.get(), R"SQL(
        DROP TABLE IF EXISTS cpu;
        CREATE TABLE cpu (
            _time TEXT NOT NULL,
            host TEXT NOT NULL,
            usage REAL NOT NULL,
            active BOOLEAN NOT NULL
        );
        INSERT INTO cpu (_time, host, usage, active) VALUES
            ('2024-07-01 10:00:00.000000', 'edge-1', 71.5, TRUE),
            ('2024-07-01 10:01:00.000000', 'edge-2', 64.0, TRUE),
            ('2024-07-01 10:02:00.000000', 'edge-3', 42.75, FALSE),
            ('2024-07-01 10:03:00.000000', 'edge-2', 82.0, TRUE);
    )SQL");
}

TEST(SQLiteSourceTest, ScansTableIntoTableValue) {
    SQLiteSource source(kMetricsDb, "cpu");

    auto value_or = source.Scan({});

    ASSERT_TRUE(value_or.ok()) << value_or.status();
    ASSERT_EQ(runtime::Value::Type::Table, value_or->type());
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

TEST(SQLiteSourceTest, ReportsTableStatistics) {
    SQLiteSource source(kMetricsDb, "cpu");

    auto statistics_or = source.Statistics();

    ASSERT_TRUE(statistics_or.ok()) << statistics_or.status();
    ASSERT_TRUE(statistics_or->row_count.has_value());
    EXPECT_EQ(4.0, *statistics_or->row_count);
    ASSERT_EQ(4, statistics_or->columns.size());
    EXPECT_EQ("_time", statistics_or->columns[0].name);
    EXPECT_TRUE(statistics_or->columns[1].distinct_values.has_value());
    EXPECT_EQ(3.0, *statistics_or->columns[1].distinct_values);
    EXPECT_TRUE(statistics_or->columns[1].null_fraction.has_value());
    EXPECT_EQ(0.0, *statistics_or->columns[1].null_fraction);
    EXPECT_TRUE(statistics_or->columns[1].average_width_bytes.has_value());
    EXPECT_GT(*statistics_or->columns[1].average_width_bytes, 0.0);
    ASSERT_FALSE(statistics_or->columns[1].most_common_values.empty());
    EXPECT_EQ("\"edge-1\"", statistics_or->columns[1].most_common_values[0].value);
    EXPECT_EQ(2.0, statistics_or->columns[1].most_common_values[0].frequency);
}

TEST(SQLiteSourceTest, RuntimeMetadataSplitAndPageSourceScansTable) {
    SourceSpec spec{.source = "sqlite", .driver = "sqlite", .dsn = kMetricsDb, .table = "cpu"};
    SQLiteConnectorMetadata metadata(spec);

    auto handle_or = metadata.GetTableHandle(spec);
    ASSERT_TRUE(handle_or.ok()) << handle_or.status();
    auto schema_or = metadata.Schema(*handle_or);
    ASSERT_TRUE(schema_or.ok()) << schema_or.status();
    ASSERT_EQ(4, schema_or->columns.size());

    ScanRequest request;
    request.columns = {"host", "usage"};
    request.order_by.push_back({.column = "host", .desc = false});
    SingleSplitManager split_manager;
    auto splits_or = split_manager.GetSplits(*handle_or, request);
    ASSERT_TRUE(splits_or.ok()) << splits_or.status();
    ASSERT_EQ(1, splits_or->size());

    SQLitePageSourceProvider provider(2);
    auto page_source_or = provider.CreatePageSource(splits_or->front());
    ASSERT_TRUE(page_source_or.ok()) << page_source_or.status();

    auto first_or = (*page_source_or)->NextPage();
    ASSERT_TRUE(first_or.ok()) << first_or.status();
    ASSERT_TRUE(first_or->has_value());
    runtime::TableValue first = TableValueFromPage(first_or->value());
    EXPECT_EQ(2, first.rows.size());
    EXPECT_EQ("\"edge-1\"", first.rows[0]->lookup("host")->string());

    auto second_or = (*page_source_or)->NextPage();
    ASSERT_TRUE(second_or.ok()) << second_or.status();
    ASSERT_TRUE(second_or->has_value());
    runtime::TableValue second = TableValueFromPage(second_or->value());
    EXPECT_EQ(2, second.rows.size());

    auto done_or = (*page_source_or)->NextPage();
    ASSERT_TRUE(done_or.ok()) << done_or.status();
    EXPECT_FALSE(done_or->has_value());
}

TEST(SQLiteSourceTest, RuntimeSplitManagerUsesRowidRangesForStreamingScan) {
    SourceSpec spec{.source = "sqlite", .driver = "sqlite", .dsn = kMetricsDb, .table = "cpu"};
    SQLiteConnectorMetadata metadata(spec);
    auto handle_or = metadata.GetTableHandle(spec);
    ASSERT_TRUE(handle_or.ok()) << handle_or.status();

    SQLiteSplitManager split_manager(4);
    ScanRequest request;
    request.columns = {"host", "usage"};
    auto splits_or = split_manager.GetSplits(*handle_or, request);
    ASSERT_TRUE(splits_or.ok()) << splits_or.status();
    ASSERT_GT(splits_or->size(), 1);
    EXPECT_TRUE(splits_or->front().rowid_lower.has_value());
    EXPECT_TRUE(splits_or->front().rowid_upper.has_value());

    SQLitePageSourceProvider provider(2);
    size_t rows = 0;
    for (const auto& split : *splits_or) {
        auto source_or = provider.CreatePageSource(split);
        ASSERT_TRUE(source_or.ok()) << source_or.status();
        while (true) {
            auto page_or = (*source_or)->NextPage();
            ASSERT_TRUE(page_or.ok()) << page_or.status();
            if (!page_or->has_value()) {
                break;
            }
            rows += page_or->value().row_count();
        }
    }
    EXPECT_EQ(4, rows);
}

TEST(SQLiteSourceTest, RuntimePageSourceEmitsSingleEmptyPage) {
    SourceSpec spec{.source = "sqlite", .driver = "sqlite", .dsn = kMetricsDb, .table = "cpu"};
    SQLiteConnectorMetadata metadata(spec);
    auto handle_or = metadata.GetTableHandle(spec);
    ASSERT_TRUE(handle_or.ok()) << handle_or.status();

    ScanRequest request;
    request.limit = 0;
    SingleSplitManager split_manager;
    auto splits_or = split_manager.GetSplits(*handle_or, request);
    ASSERT_TRUE(splits_or.ok()) << splits_or.status();
    SQLitePageSourceProvider provider(2);
    auto page_source_or = provider.CreatePageSource(splits_or->front());
    ASSERT_TRUE(page_source_or.ok()) << page_source_or.status();

    auto page_or = (*page_source_or)->NextPage();
    ASSERT_TRUE(page_or.ok()) << page_or.status();
    ASSERT_TRUE(page_or->has_value());
    EXPECT_TRUE(page_or->value().empty());

    auto done_or = (*page_source_or)->NextPage();
    ASSERT_TRUE(done_or.ok()) << done_or.status();
    EXPECT_FALSE(done_or->has_value());
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
        .literal = runtime::Value::string("edge-1"),
    });
    request.order_by.push_back({
        .column = "usage",
        .desc = true,
    });
    request.limit = 1;

    auto value_or = source.Scan(request);

    ASSERT_TRUE(value_or.ok()) << value_or.status();
    ASSERT_EQ(runtime::Value::Type::Table, value_or->type());
    const auto& table = value_or->as_table();
    ASSERT_EQ(1, table.rows.size());
    ASSERT_NE(nullptr, table.rows[0]);
    EXPECT_EQ("\"edge-1\"", table.rows[0]->lookup("host")->string());
    EXPECT_EQ("93.25", table.rows[0]->lookup("usage")->string());
    EXPECT_EQ(nullptr, table.rows[0]->lookup("_time"));
}

TEST(SQLiteSourceTest, PushesDownBooleanPredicates) {
    const std::string path = ::testing::TempDir() + "/sqlite_bool_predicate_metrics.db";
    ASSERT_NO_FATAL_FAILURE(CreateBoolMetricsDb(path));

    SQLiteSource source(path, "cpu");
    ScanRequest request;
    request.columns = {"host", "usage", "active"};
    request.predicates.push_back({
        .op = PredicateOp::Eq,
        .column = "active",
        .literal = runtime::Value::boolean(true),
    });
    request.predicates.push_back({
        .op = PredicateOp::Eq,
        .column = "host",
        .literal = runtime::Value::string("edge-2"),
    });
    request.order_by.push_back({
        .column = "_time",
        .desc = false,
    });

    auto value_or = source.Scan(request);

    ASSERT_TRUE(value_or.ok()) << value_or.status();
    ASSERT_EQ(runtime::Value::Type::Table, value_or->type());
    const auto& table = value_or->as_table();
    ASSERT_EQ(2, table.rows.size());
    EXPECT_EQ("\"edge-2\"", table.rows[0]->lookup("host")->string());
    EXPECT_EQ("64", table.rows[0]->lookup("usage")->string());
    EXPECT_EQ("1", table.rows[0]->lookup("active")->string());
    EXPECT_EQ("82", table.rows[1]->lookup("usage")->string());
    EXPECT_EQ("1", table.rows[1]->lookup("active")->string());
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
    ASSERT_EQ(runtime::Value::Type::Table, value_or->type());
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
    ASSERT_EQ(runtime::Value::Type::Table, value_or->type());
    const auto& table = value_or->as_table();
    ASSERT_EQ(3, table.rows.size());
    ASSERT_NE(nullptr, table.rows[0]);
    ASSERT_NE(nullptr, table.rows[1]);
    EXPECT_EQ("\"edge-1\"", table.rows[0]->lookup("service")->string());
    EXPECT_EQ("\"edge-2\"", table.rows[1]->lookup("service")->string());
    EXPECT_EQ("\"edge-3\"", table.rows[2]->lookup("service")->string());
    EXPECT_EQ(nullptr, table.rows[0]->lookup("host"));
}

TEST(SQLiteSourceTest, PushesDownDistinctWithoutLeakingSourceColumns) {
    SQLiteSource source(kMetricsDb, "cpu");
    ScanRequest request;
    request.distinct = "host";
    request.order_by.push_back({
        .column = "host",
        .desc = false,
    });

    auto value_or = source.Scan(request);

    ASSERT_TRUE(value_or.ok()) << value_or.status();
    ASSERT_EQ(runtime::Value::Type::Table, value_or->type());
    const auto& table = value_or->as_table();
    ASSERT_EQ(3, table.rows.size());
    ASSERT_NE(nullptr, table.rows[0]);
    EXPECT_EQ("\"edge-1\"", table.rows[0]->lookup("host")->string());
    EXPECT_EQ(nullptr, table.rows[0]->lookup("usage"));
    EXPECT_EQ(nullptr, table.rows[0]->lookup("region"));
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

TEST(SQLiteSourceTest, RejectsInvalidPushdownContractBeforeBuildingSql) {
    SQLiteSource source(kMetricsDb, "cpu");

    ScanRequest mixed_projection;
    mixed_projection.columns = {"host"};
    mixed_projection.projection_columns.push_back({
        .column = "usage",
        .alias = "usage",
    });
    auto mixed_or = source.Scan(mixed_projection);
    ASSERT_FALSE(mixed_or.ok());
    EXPECT_EQ(absl::StatusCode::kInvalidArgument, mixed_or.status().code());
    EXPECT_NE(std::string::npos,
              mixed_or.status().message().find("both columns and projection_columns"));

    ScanRequest group_without_aggregate;
    group_without_aggregate.group_by = {"host"};
    auto group_or = source.Scan(group_without_aggregate);
    ASSERT_FALSE(group_or.ok());
    EXPECT_EQ(absl::StatusCode::kInvalidArgument, group_or.status().code());
    EXPECT_NE(std::string::npos, group_or.status().message().find("group_by requires aggregate"));

    ScanRequest invalid_distinct_projection;
    invalid_distinct_projection.distinct = "host";
    invalid_distinct_projection.projection_columns.push_back({
        .column = "usage",
        .alias = "value",
    });
    auto distinct_or = source.Scan(invalid_distinct_projection);
    ASSERT_FALSE(distinct_or.ok());
    EXPECT_EQ(absl::StatusCode::kInvalidArgument, distinct_or.status().code());
    EXPECT_NE(std::string::npos,
              distinct_or.status().message().find("distinct projection must use distinct column"));
}

} // namespace
} // namespace pl::flux::connector
