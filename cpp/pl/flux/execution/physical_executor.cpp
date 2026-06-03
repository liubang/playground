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

#include "cpp/pl/flux/execution/physical_executor.h"

#include <utility>

namespace pl::flux::execution {

absl::StatusOr<SchedulerResult> PhysicalExecutor::ExecuteWithProfile(
    const std::shared_ptr<plan::PlanNode>& logical_plan) const {
    auto operator_or = PhysicalPlanner().Plan(logical_plan);
    if (!operator_or.ok()) {
        return operator_or.status();
    }
    return Scheduler().RunWithProfile(std::move(*operator_or));
}

absl::StatusOr<SchedulerStreamResult> PhysicalExecutor::ExecuteToSink(
    const std::shared_ptr<plan::PlanNode>& logical_plan, const Scheduler::PageSink& sink) const {
    auto task_or = PhysicalPlanner().Plan(logical_plan);
    if (!task_or.ok()) {
        return task_or.status();
    }
    return Scheduler().RunToSink(std::move(*task_or), sink);
}

absl::StatusOr<runtime::Value> PhysicalExecutor::Execute(
    const std::shared_ptr<plan::PlanNode>& logical_plan) const {
    auto result_or = ExecuteWithProfile(logical_plan);
    if (!result_or.ok()) {
        return result_or.status();
    }
    return std::move(result_or->value);
}

} // namespace pl::flux::execution
