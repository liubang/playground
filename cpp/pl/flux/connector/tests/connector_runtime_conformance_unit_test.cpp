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

#include "cpp/pl/flux/connector/memory_source.h"
#include "cpp/pl/flux/connector/mysql_source.h"
#include "cpp/pl/flux/connector/sqlite_source.h"
#include "gtest/gtest.h"
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pl::flux::connector {
namespace {

constexpr const char* kMetricsDb = "cpp/pl/flux/examples/cross_source/metrics.db";

std::vector<std::shared_ptr<ObjectValue>> make_rows() {
    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.push_back(std::make_shared<ObjectValue>(std::vector<std::pair<std::string, Value>>{
        {"host", Value::string("edge-1")}, {"usage", Value::floating(71.5)}}));
    rows.push_back(std::make_shared<ObjectValue>(std::vector<std::pair<std::string, Value>>{
        {"host", Value::string("edge-2")}, {"usage", Value::floating(81.0)}}));
    rows.push_back(std::make_shared<ObjectValue>(std::vector<std::pair<std::string, Value>>{
        {"host", Value::string("edge-3")}, {"usage", Value::floating(62.75)}}));
    return rows;
}

absl::StatusOr<std::vector<Page>> collect_pages(ConnectorRuntime& runtime,
                                                const SourceSpec& spec,
                                                const ScanRequest& request) {
    auto handle_or = runtime.metadata->GetTableHandle(spec);
    if (!handle_or.ok()) {
        return handle_or.status();
    }
    auto splits_or = runtime.split_manager->GetSplits(*handle_or, request);
    if (!splits_or.ok()) {
        return splits_or.status();
    }

    std::vector<Page> pages;
    for (const auto& split : *splits_or) {
        auto source_or = runtime.page_source_provider->CreatePageSource(split);
        if (!source_or.ok()) {
            return source_or.status();
        }
        while (true) {
            auto page_or = (*source_or)->NextPage();
            if (!page_or.ok()) {
                return page_or.status();
            }
            if (!page_or->has_value()) {
                break;
            }
            pages.push_back(std::move(page_or->value()));
        }
    }
    return pages;
}

void expect_basic_runtime_contract(ConnectorRuntime& runtime, const SourceSpec& spec) {
    auto handle_or = runtime.metadata->GetTableHandle(spec);
    ASSERT_TRUE(handle_or.ok()) << handle_or.status();
    EXPECT_EQ(spec.source, handle_or->source);
    EXPECT_EQ(spec.driver, handle_or->driver);
    EXPECT_EQ(spec.dsn, handle_or->dsn);
    EXPECT_EQ(spec.table, handle_or->table);

    auto schema_or = runtime.metadata->Schema(*handle_or);
    ASSERT_TRUE(schema_or.ok()) << schema_or.status();
    ASSERT_FALSE(schema_or->columns.empty());

    auto statistics_or = runtime.metadata->Statistics(*handle_or);
    ASSERT_TRUE(statistics_or.ok()) << statistics_or.status();

    auto splits_or = runtime.split_manager->GetSplits(*handle_or, {});
    ASSERT_TRUE(splits_or.ok()) << splits_or.status();
    ASSERT_FALSE(splits_or->empty());
    EXPECT_EQ(spec.table, splits_or->front().table.table);
}

TEST(ConnectorRuntimeConformanceTest, MemoryRuntimeExposesMetadataSplitsAndMultiPageSource) {
    SourceSpec spec{.source = "array", .driver = "memory", .table = "hosts"};
    auto runtime = MakeMemoryConnectorRuntime(spec, "hosts", make_rows(), 2);
    ASSERT_NE(nullptr, runtime);

    expect_basic_runtime_contract(*runtime, spec);

    auto pages_or = collect_pages(*runtime, spec, {});
    ASSERT_TRUE(pages_or.ok()) << pages_or.status();
    ASSERT_EQ(2, pages_or->size());
    TableValue first = TableValueFromPage(pages_or->at(0));
    TableValue second = TableValueFromPage(pages_or->at(1));
    ASSERT_EQ(2, first.rows.size());
    ASSERT_EQ(1, second.rows.size());
    EXPECT_EQ("\"edge-1\"", first.rows[0]->lookup("host")->string());
    EXPECT_EQ("\"edge-3\"", second.rows[0]->lookup("host")->string());
}

TEST(ConnectorRuntimeConformanceTest, PageSourceReportsSplitLifecycleStats) {
    SourceSpec spec{.source = "array", .driver = "memory", .table = "hosts"};
    auto runtime = MakeMemoryConnectorRuntime(spec, "hosts", make_rows(), 2);
    ASSERT_NE(nullptr, runtime);

    auto handle_or = runtime->metadata->GetTableHandle(spec);
    ASSERT_TRUE(handle_or.ok()) << handle_or.status();
    auto splits_or = runtime->split_manager->GetSplits(*handle_or, {});
    ASSERT_TRUE(splits_or.ok()) << splits_or.status();
    ASSERT_EQ(1, splits_or->size());
    EXPECT_EQ(0, splits_or->front().split_id);
    EXPECT_FALSE(splits_or->front().finished);

    auto source_or = runtime->page_source_provider->CreatePageSource(splits_or->front());
    ASSERT_TRUE(source_or.ok()) << source_or.status();
    EXPECT_FALSE((*source_or)->Finished());

    ASSERT_TRUE((*source_or)->NextPage().ok());
    ASSERT_TRUE((*source_or)->NextPage().ok());
    auto done_or = (*source_or)->NextPage();
    ASSERT_TRUE(done_or.ok()) << done_or.status();
    EXPECT_FALSE(done_or->has_value());

    ConnectorSplitStats stats = (*source_or)->Stats();
    EXPECT_TRUE((*source_or)->Finished());
    EXPECT_TRUE(stats.finished);
    EXPECT_EQ(0, stats.split_id);
    EXPECT_EQ(2, stats.pages_produced);
    EXPECT_EQ(3, stats.rows_produced);
}

TEST(ConnectorRuntimeConformanceTest, MemoryRuntimeEmitsSingleEmptyPage) {
    SourceSpec spec{.source = "array", .driver = "memory", .table = "hosts"};
    auto runtime = MakeMemoryConnectorRuntime(spec, "hosts", {}, 2);
    ASSERT_NE(nullptr, runtime);

    auto pages_or = collect_pages(*runtime, spec, {});
    ASSERT_TRUE(pages_or.ok()) << pages_or.status();
    ASSERT_EQ(1, pages_or->size());
    EXPECT_TRUE(pages_or->front().empty());
}

TEST(ConnectorRuntimeConformanceTest, SQLiteRuntimeExposesMetadataSplitsAndStreamingPages) {
    SourceSpec spec{.source = "sqlite", .driver = "sqlite", .dsn = kMetricsDb, .table = "cpu"};
    auto runtime = MakeSQLiteConnectorRuntime(spec);
    ASSERT_NE(nullptr, runtime);

    expect_basic_runtime_contract(*runtime, spec);

    ScanRequest request;
    request.columns = {"host", "usage"};
    request.order_by.push_back({.column = "host", .desc = false});
    auto pages_or = collect_pages(*runtime, spec, request);
    ASSERT_TRUE(pages_or.ok()) << pages_or.status();
    ASSERT_EQ(1, pages_or->size());
    TableValue table = TableValueFromPage(pages_or->front());
    ASSERT_EQ(4, table.rows.size());
    EXPECT_EQ("\"edge-1\"", table.rows[0]->lookup("host")->string());
    EXPECT_EQ(nullptr, table.rows[0]->lookup("region"));
}

TEST(ConnectorRuntimeConformanceTest, SQLiteRuntimePageSourceEmitsSingleEmptyPage) {
    SourceSpec spec{.source = "sqlite", .driver = "sqlite", .dsn = kMetricsDb, .table = "cpu"};
    auto runtime = MakeSQLiteConnectorRuntime(spec);
    ASSERT_NE(nullptr, runtime);

    ScanRequest request;
    request.limit = 0;
    auto pages_or = collect_pages(*runtime, spec, request);
    ASSERT_TRUE(pages_or.ok()) << pages_or.status();
    ASSERT_EQ(1, pages_or->size());
    EXPECT_TRUE(pages_or->front().empty());
}

TEST(ConnectorRuntimeConformanceTest, SQLiteRuntimeRejectsUnknownPageSourceColumn) {
    SourceSpec spec{.source = "sqlite", .driver = "sqlite", .dsn = kMetricsDb, .table = "cpu"};
    auto runtime = MakeSQLiteConnectorRuntime(spec);
    ASSERT_NE(nullptr, runtime);

    ScanRequest request;
    request.columns = {"missing"};
    auto pages_or = collect_pages(*runtime, spec, request);
    ASSERT_FALSE(pages_or.ok());
    EXPECT_EQ(absl::StatusCode::kInvalidArgument, pages_or.status().code());
}

TEST(ConnectorRuntimeConformanceTest, MySQLRuntimeConformsWhenIntegrationDsnIsConfigured) {
    const char* dsn = std::getenv("FLUX_MYSQL_TEST_DSN");
    if (dsn == nullptr || std::string(dsn).empty()) {
        GTEST_SKIP() << "FLUX_MYSQL_TEST_DSN is not set";
    }

    SourceSpec spec{.source = "mysql", .driver = "mysql", .dsn = dsn, .table = "cpu"};
    auto runtime = MakeMySQLConnectorRuntime(spec);
    ASSERT_NE(nullptr, runtime);

    expect_basic_runtime_contract(*runtime, spec);

    ScanRequest request;
    request.columns = {"host", "usage"};
    request.order_by.push_back({.column = "host", .desc = false});
    request.limit = 2;
    auto pages_or = collect_pages(*runtime, spec, request);
    ASSERT_TRUE(pages_or.ok()) << pages_or.status();
    ASSERT_FALSE(pages_or->empty());
    TableValue table = TableValueFromPage(pages_or->front());
    ASSERT_LE(1, table.rows.size());
    EXPECT_NE(nullptr, table.rows[0]->lookup("host"));
}

} // namespace
} // namespace pl::flux::connector
