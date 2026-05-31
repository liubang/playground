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
#include <utility>

#include "cpp/pl/flux/optimizer/rbo.h"

namespace pl::flux::optimizer {
namespace {

std::shared_ptr<plan::PlanNode> SourceScanPlan() {
    return plan::MakeSourceScan(
        "sqlite", "sqlite", "cpp/pl/flux/examples/cross_source/metrics.db", "cpu");
}

TEST(PlanNodeTest, DefaultConstructsWithMatchingMaterializeSpec) {
    plan::PlanNode node;

    EXPECT_EQ(plan::PlanNodeKind::Materialize, node.kind);
    EXPECT_TRUE(node.materialize().reason.empty());
    EXPECT_TRUE(node.materialize().builtin.empty());
}

TEST(RuleBasedOptimizerTest, KeepsLogicalPlanStableWhileRecordingDeterministicTrace) {
    std::vector<plan::PredicateSpec> predicates = {
        {.op = plan::PredicateOp::Eq,
         .column = "host",
         .literal = {.kind = plan::PredicateLiteralKind::String, .string_value = "edge-1"}},
    };
    auto input = SourceScanPlan();
    auto plan = plan::MakeLimit(
        plan::MakeSort(plan::MakeProject(plan::MakeFilter(plan::MakeRange(input,
                                                                          "2024-01-01T00:00:00Z",
                                                                          "2024-01-02T00:00:00Z"),
                                                          std::move(predicates)),
                                         {"_time", "host", "usage"}),
                       {{.column = "usage", .desc = true}}),
        10,
        0);

    auto result_or = DefaultRuleBasedOptimizer().Optimize(plan);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    EXPECT_EQ(plan, result_or->plan);
    EXPECT_EQ((std::vector<std::string>{
                  "PushLimitIntoConnectorScan",
                  "PushSortIntoConnectorScan",
                  "PushProjectionIntoConnectorScan",
                  "PushPredicateIntoConnectorScan",
                  "PushTimeRangeIntoConnectorScan",
              }),
              AppliedRuleNames(*result_or));
    ASSERT_TRUE(result_or->pushdown_plan.has_value());
    EXPECT_EQ(SourceScanPlan()->source_scan().table, result_or->pushdown_plan->source.table);
    EXPECT_EQ((std::vector<std::string>{"_time", "host", "usage"}),
              result_or->pushdown_plan->visible_columns);
    EXPECT_EQ((std::vector<std::string>{"_time", "host", "usage"}),
              result_or->pushdown_plan->source_columns);
    ASSERT_TRUE(result_or->pushdown_plan->request.time_range.has_value());
    EXPECT_EQ("2024-01-01T00:00:00Z", result_or->pushdown_plan->request.time_range->start.value());
    EXPECT_EQ("2024-01-02T00:00:00Z", result_or->pushdown_plan->request.time_range->stop.value());
    ASSERT_EQ(1, result_or->pushdown_plan->request.predicates.size());
    EXPECT_EQ(connector::PredicateOp::Eq, result_or->pushdown_plan->request.predicates[0].op);
    EXPECT_EQ("host", result_or->pushdown_plan->request.predicates[0].column);
    EXPECT_EQ("edge-1", result_or->pushdown_plan->request.predicates[0].literal.as_string());
    EXPECT_EQ(10, result_or->pushdown_plan->request.limit.value());
    ASSERT_EQ(1, result_or->pushdown_plan->request.order_by.size());
    EXPECT_EQ("usage", result_or->pushdown_plan->request.order_by[0].column);
    EXPECT_TRUE(result_or->pushdown_plan->request.order_by[0].desc);
    ASSERT_EQ(3, result_or->pushdown_plan->request.projection_columns.size());
    EXPECT_EQ("usage", result_or->pushdown_plan->request.projection_columns[2].column);
    EXPECT_EQ("usage", result_or->pushdown_plan->request.projection_columns[2].alias);
}

TEST(RuleBasedOptimizerTest, MergesPushdownTimePredicatesIntoTimeRange) {
    std::vector<plan::PredicateSpec> predicates = {
        {.op = plan::PredicateOp::Gte,
         .column = "_time",
         .literal = {.kind = plan::PredicateLiteralKind::Time,
                     .string_value = "2024-01-01T00:30:00Z"}},
        {.op = plan::PredicateOp::Lt,
         .column = "_time",
         .literal = {.kind = plan::PredicateLiteralKind::Time,
                     .string_value = "2024-01-01T01:30:00Z"}},
        {.op = plan::PredicateOp::Eq,
         .column = "host",
         .literal = {.kind = plan::PredicateLiteralKind::String, .string_value = "edge-1"}},
    };
    auto plan = plan::MakeFilter(
        plan::MakeRange(SourceScanPlan(), "2024-01-01T00:00:00Z", "2024-01-02T00:00:00Z"),
        std::move(predicates));

    auto result_or = DefaultRuleBasedOptimizer().Optimize(plan);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(result_or->pushdown_plan.has_value());
    ASSERT_TRUE(result_or->pushdown_plan->request.time_range.has_value());
    EXPECT_EQ("2024-01-01T00:30:00Z", result_or->pushdown_plan->request.time_range->start.value());
    EXPECT_EQ("2024-01-01T01:30:00Z", result_or->pushdown_plan->request.time_range->stop.value());
    ASSERT_EQ(1, result_or->pushdown_plan->request.predicates.size());
    EXPECT_EQ(connector::PredicateOp::Eq, result_or->pushdown_plan->request.predicates[0].op);
    EXPECT_EQ("host", result_or->pushdown_plan->request.predicates[0].column);
    EXPECT_EQ("edge-1", result_or->pushdown_plan->request.predicates[0].literal.as_string());
}

TEST(RuleBasedOptimizerTest, LeavesStringTimePredicatesAsFilters) {
    std::vector<plan::PredicateSpec> predicates = {
        {.op = plan::PredicateOp::Gte,
         .column = "_time",
         .literal = {.kind = plan::PredicateLiteralKind::String,
                     .string_value = "2024-01-01T00:30:00Z"}},
    };
    auto plan = plan::MakeFilter(
        plan::MakeRange(SourceScanPlan(), "2024-01-01T00:00:00Z", "2024-01-02T00:00:00Z"),
        std::move(predicates));

    auto result_or = DefaultRuleBasedOptimizer().Optimize(plan);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(result_or->pushdown_plan.has_value());
    ASSERT_TRUE(result_or->pushdown_plan->request.time_range.has_value());
    EXPECT_EQ("2024-01-01T00:00:00Z", result_or->pushdown_plan->request.time_range->start.value());
    EXPECT_EQ("2024-01-02T00:00:00Z", result_or->pushdown_plan->request.time_range->stop.value());
    ASSERT_EQ(1, result_or->pushdown_plan->request.predicates.size());
    EXPECT_EQ(connector::PredicateOp::Gte, result_or->pushdown_plan->request.predicates[0].op);
    EXPECT_EQ("_time", result_or->pushdown_plan->request.predicates[0].column);
    EXPECT_EQ("2024-01-01T00:30:00Z",
              result_or->pushdown_plan->request.predicates[0].literal.as_string());
}

TEST(RuleBasedOptimizerTest, NormalizesRedundantPushdownPredicates) {
    std::vector<plan::PredicateSpec> first_filter = {
        {.op = plan::PredicateOp::Eq,
         .column = "host",
         .literal = {.kind = plan::PredicateLiteralKind::String, .string_value = "edge-1"}},
        {.op = plan::PredicateOp::Gt,
         .column = "usage",
         .literal = {.kind = plan::PredicateLiteralKind::Float, .float_value = 70.0}},
        {.op = plan::PredicateOp::Lte,
         .column = "usage",
         .literal = {.kind = plan::PredicateLiteralKind::Float, .float_value = 95.0}},
    };
    std::vector<plan::PredicateSpec> second_filter = {
        {.op = plan::PredicateOp::Eq,
         .column = "host",
         .literal = {.kind = plan::PredicateLiteralKind::String, .string_value = "edge-1"}},
        {.op = plan::PredicateOp::Gt,
         .column = "usage",
         .literal = {.kind = plan::PredicateLiteralKind::Float, .float_value = 80.0}},
        {.op = plan::PredicateOp::Lt,
         .column = "usage",
         .literal = {.kind = plan::PredicateLiteralKind::Float, .float_value = 95.0}},
    };
    auto plan = plan::MakeFilter(plan::MakeFilter(SourceScanPlan(), std::move(first_filter)),
                                 std::move(second_filter));

    auto result_or = DefaultRuleBasedOptimizer().Optimize(plan);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(result_or->pushdown_plan.has_value());
    ASSERT_EQ(3, result_or->pushdown_plan->request.predicates.size());
    EXPECT_EQ("host", result_or->pushdown_plan->request.predicates[0].column);
    EXPECT_EQ(connector::PredicateOp::Eq, result_or->pushdown_plan->request.predicates[0].op);
    EXPECT_EQ("edge-1", result_or->pushdown_plan->request.predicates[0].literal.as_string());
    EXPECT_EQ("usage", result_or->pushdown_plan->request.predicates[1].column);
    EXPECT_EQ(connector::PredicateOp::Gt, result_or->pushdown_plan->request.predicates[1].op);
    EXPECT_EQ(80.0, result_or->pushdown_plan->request.predicates[1].literal.as_float());
    EXPECT_EQ("usage", result_or->pushdown_plan->request.predicates[2].column);
    EXPECT_EQ(connector::PredicateOp::Lt, result_or->pushdown_plan->request.predicates[2].op);
    EXPECT_EQ(95.0, result_or->pushdown_plan->request.predicates[2].literal.as_float());
}

TEST(RuleBasedOptimizerTest, NormalizesEqualityOverRedundantBounds) {
    std::vector<plan::PredicateSpec> bounds = {
        {.op = plan::PredicateOp::Gt,
         .column = "usage",
         .literal = {.kind = plan::PredicateLiteralKind::Float, .float_value = 70.0}},
        {.op = plan::PredicateOp::Lte,
         .column = "usage",
         .literal = {.kind = plan::PredicateLiteralKind::Float, .float_value = 100.0}},
    };
    std::vector<plan::PredicateSpec> equality = {
        {.op = plan::PredicateOp::Eq,
         .column = "usage",
         .literal = {.kind = plan::PredicateLiteralKind::Float, .float_value = 88.0}},
    };
    auto plan = plan::MakeFilter(plan::MakeFilter(SourceScanPlan(), std::move(bounds)),
                                 std::move(equality));

    auto result_or = DefaultRuleBasedOptimizer().Optimize(plan);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(result_or->pushdown_plan.has_value());
    ASSERT_EQ(1, result_or->pushdown_plan->request.predicates.size());
    EXPECT_EQ("usage", result_or->pushdown_plan->request.predicates[0].column);
    EXPECT_EQ(connector::PredicateOp::Eq, result_or->pushdown_plan->request.predicates[0].op);
    EXPECT_EQ(88.0, result_or->pushdown_plan->request.predicates[0].literal.as_float());
}

TEST(RuleBasedOptimizerTest, RecordsAggregatePushdownRequestWithColumnMapping) {
    auto plan = plan::MakeAggregate(
        plan::MakeGroup(plan::MakeRename(SourceScanPlan(), {{"usage", "cpu_usage"}}), {"host"}),
        plan::AggregateFunction::Mean,
        "cpu_usage");

    auto result_or = DefaultRuleBasedOptimizer().Optimize(plan);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(result_or->pushdown_plan.has_value());
    EXPECT_EQ((std::vector<std::string>{"host"}), result_or->pushdown_plan->request.group_by);
    ASSERT_TRUE(result_or->pushdown_plan->request.aggregate.has_value());
    EXPECT_EQ(connector::AggregateFunction::Mean, result_or->pushdown_plan->request.aggregate->fn);
    EXPECT_EQ("usage", result_or->pushdown_plan->request.aggregate->column);
    EXPECT_EQ("cpu_usage", result_or->pushdown_plan->request.aggregate->alias);
}

TEST(RuleBasedOptimizerTest, AllowsLimitAboveDistinctPushdown) {
    auto plan = plan::MakeLimit(plan::MakeDistinct(SourceScanPlan(), "host"), 1, 0);

    auto result_or = DefaultRuleBasedOptimizer().Optimize(plan);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(result_or->pushdown_plan.has_value());
    ASSERT_TRUE(result_or->pushdown_plan->request.distinct.has_value());
    EXPECT_EQ("host", *result_or->pushdown_plan->request.distinct);
    ASSERT_TRUE(result_or->pushdown_plan->request.limit.has_value());
    EXPECT_EQ(1, *result_or->pushdown_plan->request.limit);
}

TEST(RuleBasedOptimizerTest, DoesNotPushDistinctAcrossChildLimit) {
    auto plan = plan::MakeDistinct(plan::MakeLimit(SourceScanPlan(), 2, 0), "host");

    auto result_or = DefaultRuleBasedOptimizer().Optimize(plan);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    EXPECT_FALSE(result_or->pushdown_plan.has_value());
}

TEST(RuleBasedOptimizerTest, DoesNotPushDistinctAcrossChildSort) {
    auto plan = plan::MakeDistinct(
        plan::MakeSort(SourceScanPlan(), {{.column = "usage", .desc = true}}), "host");

    auto result_or = DefaultRuleBasedOptimizer().Optimize(plan);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    EXPECT_FALSE(result_or->pushdown_plan.has_value());
}

TEST(RuleBasedOptimizerTest, DoesNotPushAggregateAcrossChildLimit) {
    auto plan =
        plan::MakeAggregate(plan::MakeGroup(plan::MakeLimit(SourceScanPlan(), 2, 0), {"host"}),
                            plan::AggregateFunction::Count,
                            "usage");

    auto result_or = DefaultRuleBasedOptimizer().Optimize(plan);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    EXPECT_FALSE(result_or->pushdown_plan.has_value());
}

TEST(RuleBasedOptimizerTest, RejectsInvalidExecutablePushdownRequests) {
    auto source = SourceScanPlan();
    PushdownPlan plan;
    plan.source = source->source_scan();

    plan.request.group_by = {"host"};
    EXPECT_FALSE(CanExecutePushdownPlan(plan));

    plan.request.aggregate = connector::AggregateRequest{
        .fn = connector::AggregateFunction::Count,
        .column = "usage",
        .alias = "usage",
    };
    EXPECT_TRUE(CanExecutePushdownPlan(plan));

    plan.request.distinct = "host";
    EXPECT_FALSE(CanExecutePushdownPlan(plan));

    plan.request = connector::ScanRequest{};
    plan.request.partitioned_topn = true;
    plan.request.limit = 2;
    EXPECT_FALSE(CanExecutePushdownPlan(plan));

    plan.request.order_by = {{.column = "usage", .desc = true}};
    EXPECT_TRUE(CanExecutePushdownPlan(plan));

    plan.request.offset = 1;
    EXPECT_FALSE(CanExecutePushdownPlan(plan));
}

TEST(RuleBasedOptimizerTest, TracesMaterializationBarrierWithoutPushdownRules) {
    auto plan = plan::MakeMaterializeBarrier(
        plan::MakeGroup(SourceScanPlan(), {"host"}), "unsupported lazy builtin", "test");

    auto result_or = DefaultRuleBasedOptimizer().Optimize(plan);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    EXPECT_EQ(plan, result_or->plan);
    EXPECT_EQ((std::vector<std::string>{"InsertMaterializationBarrier"}),
              AppliedRuleNames(*result_or));
}

TEST(RuleBasedOptimizerTest, InsertsMaterializationBarrierBeforeNonPushableUnaryNode) {
    auto plan = plan::MakeUnaryNode(plan::PlanNodeKind::Map, SourceScanPlan());

    auto result_or = DefaultRuleBasedOptimizer().Optimize(plan);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_NE(plan, result_or->plan);
    ASSERT_EQ(plan::PlanNodeKind::Map, result_or->plan->kind);
    ASSERT_EQ(1, result_or->plan->inputs.size());
    ASSERT_NE(nullptr, result_or->plan->inputs[0]);
    EXPECT_EQ(plan::PlanNodeKind::Materialize, result_or->plan->inputs[0]->kind);
    EXPECT_EQ("unsupported lazy builtin", result_or->plan->inputs[0]->materialize().reason);
    EXPECT_EQ("Map", result_or->plan->inputs[0]->materialize().builtin);
    EXPECT_EQ((std::vector<std::string>{"InsertMaterializationBarrier"}),
              AppliedRuleNames(*result_or));
}

} // namespace
} // namespace pl::flux::optimizer
