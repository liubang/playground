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

#include <memory>
#include <string>

#include "cpp/pl/flux/plan/plan_node.h"

namespace pl::flux::optimizer {

std::string FormatLogicalPlan(const std::shared_ptr<plan::PlanNode>& plan);
std::string FormatOptimizedLogicalPlan(const std::shared_ptr<plan::PlanNode>& plan);
std::string FormatPhysicalPlan(const std::shared_ptr<plan::PlanNode>& plan);

} // namespace pl::flux::optimizer
