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
// Created: 2026/05/17 18:27

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "cpp/pl/flux/execution/physical_executor.h"
#include "cpp/pl/flux/plan/plan_node.h"

namespace pl::flux::execution {

enum class AggregatePhase {
    Single,
    Partial,
    Final,
};

class StreamingGroupOperator final : public Operator {
public:
    StreamingGroupOperator(std::shared_ptr<plan::PlanNode> plan, std::unique_ptr<Operator> input);
    StreamingGroupOperator(std::shared_ptr<plan::PlanNode> plan,
                           std::unique_ptr<Operator> input,
                           std::shared_ptr<QueryMemoryContext> memory_context,
                           std::string phase = "single");

    [[nodiscard]] std::string name() const override;
    void Cancel() override;
    void CollectSplitStats(std::vector<connector::ConnectorSplitStats>* out) const override;
    void CollectAccumulatorStats(std::vector<AccumulatorStats>* out) const override;
    absl::StatusOr<std::optional<Page>> NextPage() override;

private:
    std::shared_ptr<plan::PlanNode> plan_;
    std::unique_ptr<Operator> input_;
    std::shared_ptr<QueryMemoryContext> memory_context_;
    AccumulatorStats stats_;
    bool emitted_ = false;
};

class StreamingDistinctOperator final : public Operator {
public:
    StreamingDistinctOperator(std::shared_ptr<plan::PlanNode> plan,
                              std::unique_ptr<Operator> input);
    StreamingDistinctOperator(std::shared_ptr<plan::PlanNode> plan,
                              std::unique_ptr<Operator> input,
                              std::shared_ptr<QueryMemoryContext> memory_context,
                              std::string phase = "single");

    [[nodiscard]] std::string name() const override;
    void Cancel() override;
    void CollectSplitStats(std::vector<connector::ConnectorSplitStats>* out) const override;
    void CollectAccumulatorStats(std::vector<AccumulatorStats>* out) const override;
    absl::StatusOr<std::optional<Page>> NextPage() override;

private:
    std::shared_ptr<plan::PlanNode> plan_;
    std::unique_ptr<Operator> input_;
    std::shared_ptr<QueryMemoryContext> memory_context_;
    AccumulatorStats stats_;
    bool emitted_ = false;
};

class StreamingAggregateOperator final : public Operator {
public:
    StreamingAggregateOperator(std::shared_ptr<plan::PlanNode> plan,
                               std::unique_ptr<Operator> input);
    StreamingAggregateOperator(std::shared_ptr<plan::PlanNode> plan,
                               std::unique_ptr<Operator> input,
                               std::shared_ptr<QueryMemoryContext> memory_context);

    [[nodiscard]] std::string name() const override;
    void Cancel() override;
    void CollectSplitStats(std::vector<connector::ConnectorSplitStats>* out) const override;
    void CollectAccumulatorStats(std::vector<AccumulatorStats>* out) const override;
    absl::StatusOr<std::optional<Page>> NextPage() override;

private:
    std::shared_ptr<plan::PlanNode> plan_;
    std::unique_ptr<Operator> input_;
    std::shared_ptr<QueryMemoryContext> memory_context_;
    AccumulatorStats stats_;
    bool emitted_ = false;
};

class StreamingGroupedAggregateOperator final : public Operator {
public:
    StreamingGroupedAggregateOperator(std::shared_ptr<plan::PlanNode> aggregate_plan,
                                      std::vector<std::string> group_columns,
                                      std::unique_ptr<Operator> input,
                                      AggregatePhase phase = AggregatePhase::Single);
    StreamingGroupedAggregateOperator(std::shared_ptr<plan::PlanNode> aggregate_plan,
                                      std::vector<std::string> group_columns,
                                      std::unique_ptr<Operator> input,
                                      std::shared_ptr<QueryMemoryContext> memory_context,
                                      AggregatePhase phase = AggregatePhase::Single);

    [[nodiscard]] std::string name() const override;
    void Cancel() override;
    void CollectSplitStats(std::vector<connector::ConnectorSplitStats>* out) const override;
    void CollectAccumulatorStats(std::vector<AccumulatorStats>* out) const override;
    absl::StatusOr<std::optional<Page>> NextPage() override;

private:
    std::shared_ptr<plan::PlanNode> aggregate_plan_;
    std::vector<std::string> group_columns_;
    std::unique_ptr<Operator> input_;
    std::shared_ptr<QueryMemoryContext> memory_context_;
    AggregatePhase phase_ = AggregatePhase::Single;
    AccumulatorStats stats_;
    bool emitted_ = false;
};

} // namespace pl::flux::execution
