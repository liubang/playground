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

#pragma once

#include "absl/status/statusor.h"
#include "cpp/pl/flux/connector/connector_runtime.h"
#include "cpp/pl/flux/plan/plan_node.h"
#include "cpp/pl/flux/runtime/runtime_page.h"
#include "cpp/pl/flux/runtime/runtime_value.h"
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace pl::flux::execution {

struct AccumulatorStats {
    std::string operator_name;
    std::string mode;
    std::string phase;
    std::string key_strategy;
    size_t input_rows = 0;
    size_t output_rows = 0;
    size_t groups = 0;
    size_t memory_bytes = 0;
    size_t memory_limit_bytes = 0;
    bool memory_limited = false;
    double key_time_ms = 0.0;
    double hash_time_ms = 0.0;
    double update_time_ms = 0.0;
    double result_time_ms = 0.0;
};

class Operator {
public:
    virtual ~Operator() = default;
    [[nodiscard]] virtual std::string name() const = 0;
    virtual absl::StatusOr<std::optional<Page>> NextPage() = 0;
    virtual void Cancel() {}
    virtual void CollectSplitStats(std::vector<connector::ConnectorSplitStats>*) const {}
    virtual void CollectAccumulatorStats(std::vector<AccumulatorStats>*) const {}
};

struct Pipeline {
    struct Stats {
        mutable std::mutex mu;
        size_t pages = 0;
        size_t rows = 0;
        std::vector<connector::ConnectorSplitStats> split_stats;
        std::vector<AccumulatorStats> accumulator_stats;
        bool blocked = false;
        bool finished = false;
        std::string error;
    };

    std::string id;
    std::string name;
    std::string role;
    std::vector<std::string> dependencies;
    std::vector<std::string> operators;
    std::unique_ptr<Operator> root;
    std::vector<std::unique_ptr<Operator>> driver_roots;
    std::shared_ptr<Stats> stats = std::make_shared<Stats>();
};

struct ExecutionTask {
    std::vector<Pipeline> pipelines;
};

struct PipelineProfile {
    std::string id;
    std::string name;
    std::string role;
    std::vector<std::string> dependencies;
    std::vector<std::string> operators;
    bool blocking = false;
    size_t drivers = 1;
    size_t pages = 0;
    size_t rows = 0;
    std::vector<connector::ConnectorSplitStats> split_stats;
    std::vector<AccumulatorStats> accumulator_stats;
    bool blocked = false;
    bool finished = false;
    std::string error;
};

struct ExecutionProfile {
    std::vector<PipelineProfile> pipelines;
};

struct SchedulerResult {
    Value value;
    ExecutionProfile profile;
};

struct SchedulerStreamResult {
    ExecutionProfile profile;
};

class PhysicalPlanner {
public:
    [[nodiscard]] absl::StatusOr<ExecutionTask> Plan(
        const std::shared_ptr<plan::PlanNode>& logical_plan) const;
};

class Driver {
public:
    explicit Driver(Pipeline pipeline);

    using PageSink = std::function<absl::Status(Page)>;

    [[nodiscard]] absl::Status RunToSink(const PageSink& sink) const;
    [[nodiscard]] absl::StatusOr<Value> Run() const;

private:
    Pipeline pipeline_;
};

struct DriverTask {
    size_t pipeline_index = 0;
    size_t driver_id = 0;
    std::string pipeline_id;
    std::string role;
    Pipeline pipeline;
};

class DriverFactory {
public:
    [[nodiscard]] static std::vector<DriverTask> CreateTasks(size_t pipeline_index,
                                                             Pipeline pipeline);
};

class Scheduler {
public:
    using PageSink = Driver::PageSink;

    [[nodiscard]] absl::StatusOr<SchedulerResult> RunWithProfile(ExecutionTask task) const;
    [[nodiscard]] absl::StatusOr<SchedulerStreamResult> RunToSink(ExecutionTask task,
                                                                  const PageSink& sink) const;
    [[nodiscard]] absl::StatusOr<Value> Run(ExecutionTask task) const;
};

class PhysicalExecutor {
public:
    [[nodiscard]] absl::StatusOr<SchedulerResult> ExecuteWithProfile(
        const std::shared_ptr<plan::PlanNode>& logical_plan) const;
    [[nodiscard]] absl::StatusOr<SchedulerStreamResult> ExecuteToSink(
        const std::shared_ptr<plan::PlanNode>& logical_plan, const Scheduler::PageSink& sink) const;
    [[nodiscard]] absl::StatusOr<Value> Execute(
        const std::shared_ptr<plan::PlanNode>& logical_plan) const;
};

std::string FormatPipelinePlan(const std::shared_ptr<plan::PlanNode>& logical_plan);
std::string FormatPipelinePlanJson(const std::shared_ptr<plan::PlanNode>& logical_plan);
std::string FormatExecutionProfile(const ExecutionProfile& profile);
std::string FormatExecutionProfileJson(const ExecutionProfile& profile);

} // namespace pl::flux::execution
