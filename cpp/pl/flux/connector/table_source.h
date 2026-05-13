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
// Created: 2026/05/07 00:35

#pragma once

#include "absl/status/statusor.h"
#include "cpp/pl/flux/runtime/runtime_value.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pl::flux::connector {

struct ColumnSchema {
    std::string name;
    Value::Type type = Value::Type::Null;
    bool nullable = true;
};

struct TableSchema {
    std::vector<ColumnSchema> columns;
};

struct ColumnStatistics {
    std::string name;
    std::optional<double> distinct_values;
    std::optional<double> null_fraction;
};

struct TableStatistics {
    std::optional<double> row_count;
    std::optional<double> size_bytes;
    std::vector<ColumnStatistics> columns;
};

struct SourceCapabilities {
    bool projection = false;
    bool filter = false;
    bool time_range = false;
    bool limit = false;
    bool sort = false;
    bool aggregate = false;
    bool distinct = false;
};

struct TimeRange {
    std::optional<std::string> start;
    std::optional<std::string> stop;
};

enum class PredicateOp {
    Eq,
    NotEq,
    Lt,
    Lte,
    Gt,
    Gte,
};

struct Predicate {
    PredicateOp op = PredicateOp::Eq;
    std::string column;
    Value literal;
};

struct OrderBy {
    std::string column;
    bool desc = false;
};

struct ProjectionColumn {
    std::string column;
    std::string alias;
};

enum class AggregateFunction {
    Count,
    Sum,
    Mean,
    Min,
    Max,
};

struct AggregateRequest {
    AggregateFunction fn = AggregateFunction::Count;
    std::string column;
    std::string alias;
};

struct ScanRequest {
    std::vector<std::string> columns;
    std::vector<ProjectionColumn> projection_columns;
    std::optional<TimeRange> time_range;
    std::vector<Predicate> predicates;
    std::vector<OrderBy> order_by;
    std::vector<std::string> group_by;
    std::optional<AggregateRequest> aggregate;
    std::optional<std::string> distinct;
    std::optional<int64_t> limit;
    std::optional<int64_t> offset;
};

} // namespace pl::flux::connector
