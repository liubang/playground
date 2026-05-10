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

#include "cpp/pl/flux/execution/materializer.h"

#include "absl/status/status.h"
#include "cpp/pl/flux/optimizer/source_pushdown.h"

namespace pl::flux::execution {

absl::StatusOr<Value> Materializer::Materialize(Value value) const {
    if (value.type() != Value::Type::Table) {
        return value;
    }
    auto& table = value.as_table_mut();
    if (table.materialized) {
        return value;
    }
    if (table.plan == nullptr) {
        return absl::InvalidArgumentError("cannot materialize lazy table without a plan");
    }
    auto pushdown_or = optimizer::BuildPushdownPlan(table.plan);
    if (!pushdown_or.ok()) {
        return pushdown_or.status();
    }
    auto materialized_or = optimizer::ExecutePushdownPlan(*pushdown_or);
    if (!materialized_or.ok()) {
        return materialized_or.status();
    }
    materialized_or->as_table_mut().plan = table.plan;
    materialized_or->as_table_mut().range_start = table.range_start;
    materialized_or->as_table_mut().range_stop = table.range_stop;
    materialized_or->as_table_mut().result_name = table.result_name;
    materialized_or->as_table_mut().materialized = true;
    return *materialized_or;
}

absl::StatusOr<Value> MaterializeValue(Value value) {
    return Materializer().Materialize(std::move(value));
}

} // namespace pl::flux::execution
