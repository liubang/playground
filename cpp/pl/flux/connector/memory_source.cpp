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

#include "cpp/pl/flux/connector/memory_source.h"

#include "absl/status/status.h"
#include <unordered_set>
#include <utility>

namespace pl::flux::connector {
namespace {

TableSchema schema_from_rows(const std::vector<std::shared_ptr<ObjectValue>>& rows) {
    TableSchema schema;
    std::unordered_set<std::string> seen;
    for (const auto& row : rows) {
        if (row == nullptr) {
            continue;
        }
        for (const auto& [name, value] : row->properties) {
            if (seen.insert(name).second) {
                schema.columns.push_back({name, value.type(), value.is_null()});
            }
        }
    }
    return schema;
}

bool has_scan_pushdown(const ScanRequest& request) {
    return !request.columns.empty() || request.time_range.has_value() ||
           !request.predicates.empty() || !request.order_by.empty() || request.limit.has_value() ||
           request.offset.has_value();
}

} // namespace

ArraySource::ArraySource(std::string bucket, std::vector<std::shared_ptr<ObjectValue>> rows)
    : bucket_(std::move(bucket)), rows_(std::move(rows)) {}

absl::StatusOr<TableSchema> ArraySource::Schema() const { return schema_from_rows(rows_); }

SourceCapabilities ArraySource::Capabilities() const { return {}; }

absl::StatusOr<Value> ArraySource::Scan(const ScanRequest& request) {
    if (has_scan_pushdown(request)) {
        return absl::UnimplementedError("array source scan pushdown is not implemented");
    }
    return Value::table(bucket_, rows_);
}

CsvSource::CsvSource(std::vector<std::shared_ptr<ObjectValue>> rows) : rows_(std::move(rows)) {}

absl::StatusOr<TableSchema> CsvSource::Schema() const { return schema_from_rows(rows_); }

SourceCapabilities CsvSource::Capabilities() const { return {}; }

absl::StatusOr<Value> CsvSource::Scan(const ScanRequest& request) {
    if (has_scan_pushdown(request)) {
        return absl::UnimplementedError("csv source scan pushdown is not implemented");
    }
    return Value::table("csv", rows_);
}

} // namespace pl::flux::connector
