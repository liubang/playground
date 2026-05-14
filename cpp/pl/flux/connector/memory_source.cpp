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
#include <algorithm>
#include <cstddef>
#include <memory>
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
                schema.columns.push_back(
                    {.name = name, .type = value.type(), .nullable = value.is_null()});
            }
        }
    }
    return schema;
}

bool has_scan_pushdown(const ScanRequest& request) {
    return !request.columns.empty() || !request.projection_columns.empty() ||
           request.time_range.has_value() || !request.predicates.empty() ||
           !request.order_by.empty() || !request.group_by.empty() ||
           request.aggregate.has_value() || request.distinct.has_value() ||
           request.limit.has_value() || request.offset.has_value();
}

TableStatistics statistics_from_rows(const std::vector<std::shared_ptr<ObjectValue>>& rows) {
    TableStatistics statistics;
    statistics.row_count = static_cast<double>(rows.size());
    const auto schema = schema_from_rows(rows);
    statistics.columns.reserve(schema.columns.size());
    for (const auto& column : schema.columns) {
        statistics.columns.push_back(
            {.name = column.name, .distinct_values = {}, .null_fraction = {}});
    }
    return statistics;
}

} // namespace

ArraySource::ArraySource(std::string bucket, std::vector<std::shared_ptr<ObjectValue>> rows)
    : bucket_(std::move(bucket)), rows_(std::move(rows)) {}

absl::StatusOr<TableSchema> ArraySource::Schema() const { return schema_from_rows(rows_); }

SourceCapabilities ArraySource::Capabilities() const { return {}; }

absl::StatusOr<TableStatistics> ArraySource::Statistics() const {
    return statistics_from_rows(rows_);
}

absl::StatusOr<Value> ArraySource::Scan(const ScanRequest& request) {
    if (has_scan_pushdown(request)) {
        return absl::UnimplementedError("array source scan pushdown is not implemented");
    }
    return Value::table(bucket_, rows_);
}

CsvSource::CsvSource(std::vector<std::shared_ptr<ObjectValue>> rows) : rows_(std::move(rows)) {}

absl::StatusOr<TableSchema> CsvSource::Schema() const { return schema_from_rows(rows_); }

SourceCapabilities CsvSource::Capabilities() const { return {}; }

absl::StatusOr<TableStatistics> CsvSource::Statistics() const {
    return statistics_from_rows(rows_);
}

absl::StatusOr<Value> CsvSource::Scan(const ScanRequest& request) {
    if (has_scan_pushdown(request)) {
        return absl::UnimplementedError("csv source scan pushdown is not implemented");
    }
    return Value::table("csv", rows_);
}

MemoryConnectorMetadata::MemoryConnectorMetadata(SourceSpec spec,
                                                 std::string bucket,
                                                 std::vector<std::shared_ptr<ObjectValue>> rows)
    : spec_(std::move(spec)), bucket_(std::move(bucket)), rows_(std::move(rows)) {}

absl::StatusOr<TableHandle> MemoryConnectorMetadata::GetTableHandle(const SourceSpec& spec) const {
    return TableHandle{
        .source = spec.source,
        .driver = spec.driver,
        .dsn = spec.dsn,
        .table = spec.table.empty() ? bucket_ : spec.table,
    };
}

absl::StatusOr<TableSchema> MemoryConnectorMetadata::Schema(const TableHandle& table) const {
    (void)table;
    return schema_from_rows(rows_);
}

SourceCapabilities MemoryConnectorMetadata::Capabilities(const TableHandle& table) const {
    (void)table;
    return {};
}

absl::StatusOr<TableStatistics> MemoryConnectorMetadata::Statistics(
    const TableHandle& table) const {
    (void)table;
    return statistics_from_rows(rows_);
}

MemoryPageSourceProvider::MemoryPageSourceProvider(std::string bucket,
                                                   std::vector<std::shared_ptr<ObjectValue>> rows,
                                                   size_t rows_per_page)
    : bucket_(std::move(bucket)),
      rows_(std::move(rows)),
      rows_per_page_(std::max<size_t>(1, rows_per_page)) {}

absl::StatusOr<std::unique_ptr<ConnectorPageSource>> MemoryPageSourceProvider::CreatePageSource(
    const ConnectorSplit& split) const {
    if (has_scan_pushdown(split.request)) {
        return absl::UnimplementedError("memory source scan pushdown is not implemented");
    }
    TableChunk chunk;
    chunk.rows = rows_;
    std::vector<TableChunk> chunks;
    chunks.push_back(std::move(chunk));
    return std::unique_ptr<ConnectorPageSource>(
        new ChunkedPageSource(bucket_, std::move(chunks), rows_per_page_, split.split_id));
}

std::unique_ptr<ConnectorRuntime> MakeMemoryConnectorRuntime(
    const SourceSpec& spec,
    std::string bucket,
    std::vector<std::shared_ptr<ObjectValue>> rows,
    size_t rows_per_page) {
    auto runtime = std::make_unique<ConnectorRuntime>();
    runtime->metadata = std::make_unique<MemoryConnectorMetadata>(spec, bucket, rows);
    runtime->split_manager = std::make_unique<SingleSplitManager>();
    runtime->page_source_provider = std::make_unique<MemoryPageSourceProvider>(
        std::move(bucket), std::move(rows), rows_per_page);
    return runtime;
}

} // namespace pl::flux::connector
