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

#include "cpp/pl/flux/optimizer/rbo.h"
#include <gtest/gtest.h>
#include <utility>

namespace pl::flux::optimizer {
namespace {

std::shared_ptr<plan::PlanNode> SourceScanPlan() {
    return plan::MakeSourceScan("sqlite", "sqlite", "metrics.db", "cpu");
}

TEST(RuleBasedOptimizerTest, KeepsLogicalPlanStableWhileRecordingDeterministicTrace) {
    std::vector<plan::PredicateSpec> predicates = {
        {.op = plan::PredicateOp::Eq,
         .column = "host",
         .literal = {.kind = plan::PredicateLiteralKind::String, .string_value = "edge-1"}},
    };
    auto input = SourceScanPlan();
    auto plan = plan::MakeLimit(
        plan::MakeSort(
            plan::MakeProject(plan::MakeFilter(plan::MakeRange(input, "2024-01-01T00:00:00Z",
                                                               "2024-01-02T00:00:00Z"),
                                               std::move(predicates)),
                              {"_time", "host", "usage"}),
            {{.column = "usage", .desc = true}}),
        10, 0);

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
}

TEST(RuleBasedOptimizerTest, TracesMaterializationBarrierWithoutPushdownRules) {
    auto plan = plan::MakeMaterializeBarrier(plan::MakeGroup(SourceScanPlan(), {"host"}),
                                             "unsupported lazy builtin", "test");

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
    EXPECT_EQ("unsupported lazy builtin", result_or->plan->inputs[0]->materialize.reason);
    EXPECT_EQ("Map", result_or->plan->inputs[0]->materialize.builtin);
    EXPECT_EQ((std::vector<std::string>{"InsertMaterializationBarrier"}),
              AppliedRuleNames(*result_or));
}

} // namespace
} // namespace pl::flux::optimizer
