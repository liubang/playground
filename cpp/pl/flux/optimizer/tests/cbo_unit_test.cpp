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

#include "cpp/pl/flux/optimizer/cbo.h"
#include <gtest/gtest.h>
#include <utility>

namespace pl::flux::optimizer {
namespace {

std::shared_ptr<plan::PlanNode> SourceScanPlan() {
    return plan::MakeSourceScan("sqlite", "sqlite", "cpp/pl/flux/examples/cross_source/metrics.db",
                                "cpu");
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
    ASSERT_EQ(2, result_or->alternatives.size());
    EXPECT_EQ(PhysicalShape::ConnectorScan, result_or->alternatives[0].shape);
    EXPECT_TRUE(result_or->alternatives[0].chosen);
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
    EXPECT_GT(*result_or->cost.rows, 0.0);
    ASSERT_EQ(2, result_or->alternatives.size());
    EXPECT_EQ(PhysicalShape::ConnectorPrefixMemorySuffix, result_or->alternatives[0].shape);
    ASSERT_TRUE(result_or->rbo_result.pushdown_plan.has_value());
    EXPECT_FALSE(CanExecutePushdownPlan(*result_or->rbo_result.pushdown_plan));
}

TEST(CostBasedOptimizerTest, EnumeratesLocalHashJoinAlternative) {
    auto logical_plan =
        plan::MakeJoin(plan::MakeProject(SourceScanPlan(), {"host", "usage"}),
                       plan::MakeProject(SourceScanPlan(), {"host", "region"}), {"host"});

    auto result_or = DefaultCostBasedOptimizer().OptimizeWithTrace(logical_plan);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    EXPECT_EQ("chosen", result_or->decision);
    ASSERT_TRUE(result_or->cost.rows.has_value());
    ASSERT_FALSE(result_or->alternatives.empty());
    EXPECT_EQ(PhysicalShape::LocalHashJoin, result_or->alternatives[0].shape);
    EXPECT_TRUE(result_or->alternatives[0].chosen);
    EXPECT_GE(result_or->alternatives.size(), 2);
}

} // namespace
} // namespace pl::flux::optimizer
