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
// Created: 2026/05/10 00:00

#include <gtest/gtest.h>
#include <memory>
#include <sqlite3.h>
#include <string>
#include <utility>

#include "cpp/pl/flux/optimizer/cbo.h"

namespace pl::flux::optimizer {
namespace {

std::shared_ptr<plan::PlanNode> SourceScanPlan() {
    return plan::MakeSourceScan(
        "sqlite", "sqlite", "cpp/pl/flux/examples/cross_source/metrics.db", "cpu");
}

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

void CreateNullableMetricsDb(const std::string& path) {
    sqlite3* raw_db = nullptr;
    ASSERT_EQ(SQLITE_OK,
              sqlite3_open_v2(
                  path.c_str(), &raw_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr));
    SqliteTestDb db(raw_db);
    ExecuteSql(db.get(), R"SQL(
        DROP TABLE IF EXISTS cpu;
        CREATE TABLE cpu (
            _time TEXT NOT NULL,
            note TEXT NULL
        );
        INSERT INTO cpu (_time, note) VALUES
            ('2024-07-01 10:00:00.000000', 'steady'),
            ('2024-07-01 10:01:00.000000', NULL),
            ('2024-07-01 10:02:00.000000', 'idle'),
            ('2024-07-01 10:03:00.000000', NULL);
    )SQL");
}

void CreateJoinCardinalityDb(const std::string& path) {
    sqlite3* raw_db = nullptr;
    ASSERT_EQ(SQLITE_OK,
              sqlite3_open_v2(
                  path.c_str(), &raw_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr));
    SqliteTestDb db(raw_db);
    ExecuteSql(db.get(), R"SQL(
        DROP TABLE IF EXISTS probe;
        DROP TABLE IF EXISTS build;
        CREATE TABLE probe (host TEXT NOT NULL);
        CREATE TABLE build (host TEXT NOT NULL);
        INSERT INTO probe (host) VALUES ('edge-hot'), ('edge-hot'), ('edge-hot'), ('edge-hot');
        INSERT INTO build (host) VALUES ('edge-hot');
    )SQL");
}

TEST(CostBasedOptimizerTest, ChoosesConnectorScanWhenStatsAreAvailable) {
    std::vector<plan::PredicateSpec> predicates = {
        {.op = plan::PredicateOp::Eq,
         .column = "host",
         .literal = {.kind = plan::PredicateLiteralKind::String, .string_value = "edge-1"}},
    };
    auto logical_plan =
        plan::MakeLimit(plan::MakeFilter(SourceScanPlan(), std::move(predicates)), 1, 0);

    auto result_or = DefaultCostBasedOptimizer().OptimizeWithTrace(logical_plan);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    EXPECT_EQ("chosen", result_or->decision);
    ASSERT_TRUE(result_or->cost.rows.has_value());
    EXPECT_EQ(1.0, *result_or->cost.rows);
    ASSERT_EQ(3, result_or->alternatives.size());
    EXPECT_EQ(PhysicalShape::ConnectorScan, result_or->alternatives[0].shape);
    EXPECT_TRUE(result_or->alternatives[0].chosen);
    EXPECT_EQ("connector_prefix_memory_suffix_after_Filter", result_or->alternatives[1].name);
    EXPECT_EQ(PhysicalShape::ConnectorPrefixMemorySuffix, result_or->alternatives[1].shape);
    ASSERT_NE(nullptr, result_or->alternatives[1].plan);
    ASSERT_EQ(plan::PlanNodeKind::Limit, result_or->alternatives[1].plan->kind);
    ASSERT_EQ(1, result_or->alternatives[1].plan->inputs.size());
    ASSERT_NE(nullptr, result_or->alternatives[1].plan->inputs[0]);
    EXPECT_EQ(plan::PlanNodeKind::Materialize, result_or->alternatives[1].plan->inputs[0]->kind);
    EXPECT_EQ("connector_prefix_memory_suffix_after_SourceScan", result_or->alternatives[2].name);
    ASSERT_TRUE(result_or->alternatives[0].cost.cpu.has_value());
    ASSERT_TRUE(result_or->alternatives[1].cost.cpu.has_value());
    EXPECT_LT(*result_or->alternatives[0].cost.cpu, *result_or->alternatives[1].cost.cpu);
    ASSERT_TRUE(result_or->rbo_result.pushdown_plan.has_value());
}

TEST(CostBasedOptimizerTest, KeepsRboFastPathWithoutConnectorStats) {
    auto logical_plan = plan::MakeLimit(SourceScanPlan(), 1, 0);

    auto result_or = FastCostBasedOptimizer().OptimizeWithTrace(logical_plan);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    EXPECT_EQ("fallback-rbo", result_or->decision);
    EXPECT_FALSE(result_or->cost.rows.has_value());
    ASSERT_EQ(2, result_or->alternatives.size());
    EXPECT_EQ(PhysicalShape::ConnectorScan, result_or->alternatives[0].shape);
    ASSERT_TRUE(result_or->rbo_result.pushdown_plan.has_value());
}

TEST(CostBasedOptimizerTest, ReportsNoStatsWhenConnectorMetadataIsUnavailable) {
    auto logical_plan = plan::MakeSourceScan("unknown", "unknown", "", "missing");

    auto result_or = DefaultCostBasedOptimizer().OptimizeWithTrace(logical_plan);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    EXPECT_EQ("no-stats", result_or->decision);
    EXPECT_FALSE(result_or->cost.rows.has_value());
    ASSERT_EQ(1, result_or->alternatives.size());
    EXPECT_EQ(PhysicalShape::MemoryScan, result_or->alternatives[0].shape);
}

TEST(CostBasedOptimizerTest, CostsMemorySuffixAfterConnectorPrefix) {
    auto logical_plan = plan::MakeGroup(SourceScanPlan(), {"host"});

    auto result_or = DefaultCostBasedOptimizer().OptimizeWithTrace(logical_plan);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    EXPECT_EQ("chosen", result_or->decision);
    ASSERT_TRUE(result_or->cost.rows.has_value());
    EXPECT_EQ(3.0, *result_or->cost.rows);
    ASSERT_EQ(1, result_or->alternatives.size());
    EXPECT_EQ(PhysicalShape::ConnectorPrefixMemorySuffix, result_or->alternatives[0].shape);
    ASSERT_TRUE(result_or->rbo_result.pushdown_plan.has_value());
    EXPECT_FALSE(CanExecutePushdownPlan(*result_or->rbo_result.pushdown_plan));
}

TEST(CostBasedOptimizerTest, UsesColumnStatisticsForPredicateSelectivity) {
    auto equality = plan::MakeFilter(
        SourceScanPlan(),
        {{.op = plan::PredicateOp::Eq,
          .column = "host",
          .literal = {.kind = plan::PredicateLiteralKind::String, .string_value = "edge-1"}}});
    auto inequality = plan::MakeFilter(
        SourceScanPlan(),
        {{.op = plan::PredicateOp::NotEq,
          .column = "region",
          .literal = {.kind = plan::PredicateLiteralKind::String, .string_value = "west"}}});

    auto equality_or = DefaultCostBasedOptimizer().OptimizeWithTrace(equality);
    auto inequality_or = DefaultCostBasedOptimizer().OptimizeWithTrace(inequality);

    ASSERT_TRUE(equality_or.ok()) << equality_or.status();
    ASSERT_TRUE(equality_or->cost.rows.has_value());
    EXPECT_NEAR(4.0 / 3.0, *equality_or->cost.rows, 1e-9);
    ASSERT_TRUE(inequality_or.ok()) << inequality_or.status();
    ASSERT_TRUE(inequality_or->cost.rows.has_value());
    EXPECT_EQ(2.0, *inequality_or->cost.rows);
}

TEST(CostBasedOptimizerTest, UsesColumnStatisticsForDistinctCardinality) {
    auto logical_plan = plan::MakeDistinct(SourceScanPlan(), "region");

    auto result_or = DefaultCostBasedOptimizer().OptimizeWithTrace(logical_plan);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(result_or->cost.rows.has_value());
    EXPECT_EQ(2.0, *result_or->cost.rows);
}

TEST(CostBasedOptimizerTest, AccountsForNullFractionInPredicatesAndDistinct) {
    const std::string path = ::testing::TempDir() + "/cbo_nullable_metrics.db";
    CreateNullableMetricsDb(path);
    auto source = [&] {
        return plan::MakeSourceScan("sqlite", "sqlite", path, "cpu");
    };
    auto predicate = [](plan::PredicateOp op) {
        return plan::PredicateSpec{
            .op = op,
            .column = "note",
            .literal = {.kind = plan::PredicateLiteralKind::String, .string_value = "steady"},
        };
    };

    auto equality_or = DefaultCostBasedOptimizer().OptimizeWithTrace(
        plan::MakeFilter(source(), {predicate(plan::PredicateOp::Eq)}));
    auto inequality_or = DefaultCostBasedOptimizer().OptimizeWithTrace(
        plan::MakeFilter(source(), {predicate(plan::PredicateOp::NotEq)}));
    auto distinct_or =
        DefaultCostBasedOptimizer().OptimizeWithTrace(plan::MakeDistinct(source(), "note"));

    ASSERT_TRUE(equality_or.ok()) << equality_or.status();
    ASSERT_TRUE(equality_or->cost.rows.has_value());
    EXPECT_EQ(1.0, *equality_or->cost.rows);
    ASSERT_TRUE(inequality_or.ok()) << inequality_or.status();
    ASSERT_TRUE(inequality_or->cost.rows.has_value());
    EXPECT_EQ(1.0, *inequality_or->cost.rows);
    ASSERT_TRUE(distinct_or.ok()) << distinct_or.status();
    ASSERT_TRUE(distinct_or->cost.rows.has_value());
    EXPECT_EQ(3.0, *distinct_or->cost.rows);
}

TEST(CostBasedOptimizerTest, PreservesColumnStatisticsAcrossProjectAndRename) {
    auto logical_plan = plan::MakeGroup(
        plan::MakeRename(plan::MakeProject(SourceScanPlan(), {"host"}), {{"host", "node"}}),
        {"node"});

    auto result_or = DefaultCostBasedOptimizer().OptimizeWithTrace(logical_plan);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(result_or->cost.rows.has_value());
    EXPECT_EQ(3.0, *result_or->cost.rows);
}

TEST(CostBasedOptimizerTest, EnumeratesLocalHashJoinAlternative) {
    auto logical_plan = plan::MakeJoin(plan::MakeProject(SourceScanPlan(), {"host", "usage"}),
                                       plan::MakeProject(SourceScanPlan(), {"host", "region"}),
                                       {"host"});

    auto result_or = DefaultCostBasedOptimizer().OptimizeWithTrace(logical_plan);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    EXPECT_EQ("chosen", result_or->decision);
    ASSERT_TRUE(result_or->cost.rows.has_value());
    ASSERT_FALSE(result_or->alternatives.empty());
    EXPECT_EQ(PhysicalShape::LocalHashJoin, result_or->alternatives[0].shape);
    EXPECT_TRUE(result_or->alternatives[0].chosen);
    EXPECT_GE(result_or->alternatives.size(), 2);
}

TEST(CostBasedOptimizerTest, ChoosesSmallerJoinBuildSide) {
    auto logical_plan = plan::MakeJoin(
        plan::MakeLimit(plan::MakeProject(SourceScanPlan(), {"host", "usage"}), 1, 0),
        plan::MakeProject(SourceScanPlan(), {"host", "region"}),
        {"host"});

    auto result_or = DefaultCostBasedOptimizer().OptimizeWithTrace(logical_plan);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_NE(nullptr, result_or->rbo_result.plan);
    ASSERT_EQ(plan::PlanNodeKind::Join, result_or->rbo_result.plan->kind);
    EXPECT_EQ(plan::JoinBuildSide::Left, result_or->rbo_result.plan->join().build_side);
    ASSERT_GE(result_or->alternatives.size(), 2);
    EXPECT_EQ("local_hash_join_build_left", result_or->alternatives[0].name);
    EXPECT_TRUE(result_or->alternatives[0].chosen);
    EXPECT_EQ("local_hash_join_build_right", result_or->alternatives[1].name);
    EXPECT_FALSE(result_or->alternatives[1].chosen);
}

TEST(CostBasedOptimizerTest, ChoosesSmallerJoinBuildSideBelowUnaryWrapper) {
    auto logical_plan = plan::MakeProject(
        plan::MakeJoin(
            plan::MakeLimit(plan::MakeProject(SourceScanPlan(), {"host", "usage"}), 1, 0),
            plan::MakeProject(SourceScanPlan(), {"host", "region"}),
            {"host"}),
        {"host", "usage", "region"});

    auto result_or = DefaultCostBasedOptimizer().OptimizeWithTrace(logical_plan);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_NE(nullptr, result_or->rbo_result.plan);
    ASSERT_EQ(plan::PlanNodeKind::Project, result_or->rbo_result.plan->kind);
    ASSERT_EQ(1, result_or->rbo_result.plan->inputs.size());
    ASSERT_NE(nullptr, result_or->rbo_result.plan->inputs[0]);
    ASSERT_EQ(plan::PlanNodeKind::Join, result_or->rbo_result.plan->inputs[0]->kind);
    EXPECT_EQ(plan::JoinBuildSide::Left, result_or->rbo_result.plan->inputs[0]->join().build_side);
}

TEST(CostBasedOptimizerTest, UsesColumnStatisticsForManyToOneJoinCardinality) {
    const std::string path = ::testing::TempDir() + "/cbo_join_cardinality.db";
    CreateJoinCardinalityDb(path);
    auto scan = [&](const std::string& table) {
        return plan::MakeSourceScan("sqlite", "sqlite", path, table);
    };

    auto inner_or = DefaultCostBasedOptimizer().OptimizeWithTrace(
        plan::MakeJoin(scan("probe"), scan("build"), {"host"}));
    auto left_or = DefaultCostBasedOptimizer().OptimizeWithTrace(
        plan::MakeJoin(scan("probe"), scan("build"), {"host"}, plan::JoinMethod::Left));

    ASSERT_TRUE(inner_or.ok()) << inner_or.status();
    ASSERT_TRUE(inner_or->cost.rows.has_value());
    EXPECT_EQ(4.0, *inner_or->cost.rows);
    ASSERT_TRUE(left_or.ok()) << left_or.status();
    ASSERT_TRUE(left_or->cost.rows.has_value());
    EXPECT_EQ(4.0, *left_or->cost.rows);
}

} // namespace
} // namespace pl::flux::optimizer
