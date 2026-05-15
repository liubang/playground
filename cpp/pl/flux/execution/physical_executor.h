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
#include "cpp/pl/flux/plan/plan_node.h"
#include "cpp/pl/flux/runtime/runtime_page.h"
#include "cpp/pl/flux/runtime/runtime_value.h"
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pl::flux::execution {

class Operator {
public:
    virtual ~Operator() = default;
    [[nodiscard]] virtual std::string name() const = 0;
    virtual absl::StatusOr<std::optional<Page>> NextPage() = 0;
};

struct Pipeline {
    struct Stats {
        size_t pages = 0;
        size_t rows = 0;
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
    size_t pages = 0;
    size_t rows = 0;
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

class PhysicalPlanner {
public:
    [[nodiscard]] absl::StatusOr<ExecutionTask> Plan(
        const std::shared_ptr<plan::PlanNode>& logical_plan) const;
};

class Driver {
public:
    explicit Driver(Pipeline pipeline);

    [[nodiscard]] absl::StatusOr<Value> Run() const;

private:
    Pipeline pipeline_;
};

class Scheduler {
public:
    [[nodiscard]] absl::StatusOr<SchedulerResult> RunWithProfile(ExecutionTask task) const;
    [[nodiscard]] absl::StatusOr<Value> Run(ExecutionTask task) const;
};

class PhysicalExecutor {
public:
    [[nodiscard]] absl::StatusOr<SchedulerResult> ExecuteWithProfile(
        const std::shared_ptr<plan::PlanNode>& logical_plan) const;
    [[nodiscard]] absl::StatusOr<Value> Execute(
        const std::shared_ptr<plan::PlanNode>& logical_plan) const;
};

std::string FormatPipelinePlan(const std::shared_ptr<plan::PlanNode>& logical_plan);
std::string FormatExecutionProfile(const ExecutionProfile& profile);

} // namespace pl::flux::execution
