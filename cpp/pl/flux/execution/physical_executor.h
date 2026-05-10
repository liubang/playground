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
#include "cpp/pl/flux/runtime/runtime_value.h"
#include <memory>

namespace pl::flux::execution {

class Operator {
public:
    virtual ~Operator() = default;
    virtual absl::StatusOr<Value> Next() = 0;
};

class PhysicalPlanner {
public:
    [[nodiscard]] absl::StatusOr<std::unique_ptr<Operator>> Plan(
        const std::shared_ptr<plan::PlanNode>& logical_plan) const;
};

class Driver {
public:
    explicit Driver(std::unique_ptr<Operator> root);

    absl::StatusOr<Value> Run();

private:
    std::unique_ptr<Operator> root_;
};

class PhysicalExecutor {
public:
    [[nodiscard]] absl::StatusOr<Value> Execute(
        const std::shared_ptr<plan::PlanNode>& logical_plan) const;
};

} // namespace pl::flux::execution
