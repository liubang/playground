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
#include "cpp/pl/flux/optimizer/rbo.h"
#include "cpp/pl/flux/plan/plan_node.h"
#include "cpp/pl/flux/runtime_value.h"
#include <memory>
#include <optional>
#include <string>

namespace pl::flux::optimizer {

absl::StatusOr<PushdownPlan> BuildPushdownPlan(const std::shared_ptr<plan::PlanNode>& node);

bool CanExecutePushdownPlan(const PushdownPlan& plan);

std::string FormatPushdownRequest(const connector::ScanRequest& request);

std::optional<std::string> SourcePushdownSummary(const std::shared_ptr<plan::PlanNode>& plan);

absl::StatusOr<Value> ExecutePushdownPlan(const PushdownPlan& plan);

} // namespace pl::flux::optimizer
