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
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
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

bool has_unsupported_memory_pushdown(const ScanRequest& request) {
    return request.time_range.has_value() || !request.order_by.empty() ||
           !request.group_by.empty() || request.aggregate.has_value() ||
           request.distinct.has_value();
}

double numeric_value(const Value& value) {
    switch (value.type()) {
        case Value::Type::Int:
            return static_cast<double>(value.as_int());
        case Value::Type::UInt:
            return static_cast<double>(value.as_uint());
        case Value::Type::Float:
            return value.as_float();
        default:
            return 0.0;
    }
}

bool is_numeric_type(Value::Type type) {
    return type == Value::Type::Int || type == Value::Type::UInt || type == Value::Type::Float;
}

int compare_memory_values(const Value& lhs, const Value& rhs) {
    if (lhs.is_null() && rhs.is_null()) {
        return 0;
    }
    if (lhs.is_null()) {
        return -1;
    }
    if (rhs.is_null()) {
        return 1;
    }
    if (is_numeric_type(lhs.type()) && is_numeric_type(rhs.type())) {
        const double left = numeric_value(lhs);
        const double right = numeric_value(rhs);
        if (left < right) {
            return -1;
        }
        if (left > right) {
            return 1;
        }
        return 0;
    }
    const std::string left = lhs.string();
    const std::string right = rhs.string();
    if (left < right) {
        return -1;
    }
    if (left > right) {
        return 1;
    }
    return 0;
}

bool predicate_matches_row(const ObjectValue& row, const Predicate& predicate) {
    const Value* value = row.lookup(predicate.column);
    const Value lhs = value == nullptr ? Value::null() : *value;
    const int cmp = compare_memory_values(lhs, predicate.literal);
    switch (predicate.op) {
        case PredicateOp::Eq:
            return cmp == 0;
        case PredicateOp::NotEq:
            return cmp != 0;
        case PredicateOp::Lt:
            return cmp < 0;
        case PredicateOp::Lte:
            return cmp <= 0;
        case PredicateOp::Gt:
            return cmp > 0;
        case PredicateOp::Gte:
            return cmp >= 0;
    }
    return false;
}

bool row_matches_request(const ObjectValue& row, const ScanRequest& request) {
    for (const auto& predicate : request.predicates) {
        if (!predicate_matches_row(row, predicate)) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> projection_columns(const ScanRequest& request,
                                            const std::vector<std::shared_ptr<ObjectValue>>& rows) {
    std::vector<std::string> columns;
    if (!request.projection_columns.empty()) {
        columns.reserve(request.projection_columns.size());
        for (const auto& projection : request.projection_columns) {
            columns.push_back(projection.alias.empty() ? projection.column : projection.alias);
        }
        return columns;
    }
    if (!request.columns.empty()) {
        return request.columns;
    }
    TableSchema schema = schema_from_rows(rows);
    columns.reserve(schema.columns.size());
    for (const auto& column : schema.columns) {
        columns.push_back(column.name);
    }
    return columns;
}

Value projected_value(const ObjectValue& row,
                      const ScanRequest& request,
                      const std::string& output_name,
                      size_t projection_index) {
    if (!request.projection_columns.empty()) {
        const auto& projection = request.projection_columns[projection_index];
        const Value* value = row.lookup(projection.column);
        return value == nullptr ? Value::null() : *value;
    }
    const Value* value = row.lookup(output_name);
    return value == nullptr ? Value::null() : *value;
}

TableStatistics statistics_from_rows(const std::vector<std::shared_ptr<ObjectValue>>& rows) {
    TableStatistics statistics;
    statistics.row_count = static_cast<double>(rows.size());
    const auto schema = schema_from_rows(rows);
    statistics.columns.reserve(schema.columns.size());
    for (const auto& column : schema.columns) {
        size_t null_count = 0;
        size_t non_null_count = 0;
        size_t total_width = 0;
        std::unordered_set<std::string> distinct_values;
        for (const auto& row : rows) {
            const Value* value = row == nullptr ? nullptr : row->lookup(column.name);
            if (value == nullptr || value->is_null()) {
                ++null_count;
                continue;
            }
            const std::string encoded = value->string();
            distinct_values.insert(encoded);
            total_width += encoded.size();
            ++non_null_count;
        }
        statistics.columns.push_back(
            {.name = column.name,
             .distinct_values = static_cast<double>(distinct_values.size()),
             .null_fraction =
                 rows.empty() ? 0.0
                              : static_cast<double>(null_count) / static_cast<double>(rows.size()),
             .average_width_bytes = non_null_count == 0 ? 0.0
                                                        : static_cast<double>(total_width) /
                                                              static_cast<double>(non_null_count)});
    }
    return statistics;
}

class MemoryPageSource final : public ConnectorPageSource {
public:
    MemoryPageSource(std::string bucket,
                     std::vector<std::shared_ptr<ObjectValue>> rows,
                     ScanRequest request,
                     size_t start,
                     size_t count,
                     size_t rows_per_page,
                     int64_t split_id)
        : bucket_(std::move(bucket)),
          rows_(std::move(rows)),
          request_(std::move(request)),
          next_(std::min(start, rows_.size())),
          end_(std::min(rows_.size(), start + count)),
          rows_per_page_(std::max<size_t>(1, rows_per_page)),
          columns_(projection_columns(request_, rows_)) {
        stats_.split_id = split_id;
    }

    absl::StatusOr<std::optional<Page>> NextPage() override {
        if (finished_) {
            return std::nullopt;
        }

        while (true) {
            PageChunk chunk;
            chunk.columns.reserve(columns_.size());
            for (const auto& name : columns_) {
                chunk.columns.push_back(ColumnVector{.name = name});
            }

            while (next_ < end_ && chunk.row_count < rows_per_page_) {
                const auto& row = rows_[next_++];
                if (row == nullptr || !row_matches_request(*row, request_)) {
                    continue;
                }
                if (remaining_offset_ > 0) {
                    --remaining_offset_;
                    continue;
                }
                if (remaining_limit_.has_value() && *remaining_limit_ == 0) {
                    break;
                }
                for (size_t index = 0; index < columns_.size(); ++index) {
                    Value value = projected_value(*row, request_, columns_[index], index);
                    auto& column = chunk.columns[index];
                    if (column.type == Value::Type::Null && !value.is_null()) {
                        column.type = value.type();
                    }
                    column.values.push_back(std::move(value));
                }
                ++chunk.row_count;
                if (remaining_limit_.has_value()) {
                    --*remaining_limit_;
                }
            }

            if (chunk.row_count == 0) {
                if (next_ >= end_ || (remaining_limit_.has_value() && *remaining_limit_ == 0)) {
                    finished_ = true;
                    stats_.finished = true;
                    if (!emitted_any_page_) {
                        emitted_any_page_ = true;
                        Page empty;
                        empty.bucket = bucket_;
                        empty.chunks.push_back(std::move(chunk));
                        ++stats_.pages_produced;
                        return empty;
                    }
                    return std::nullopt;
                }
                continue;
            }

            Page page;
            page.bucket = bucket_;
            page.chunks.push_back(std::move(chunk));
            emitted_any_page_ = true;
            ++stats_.pages_produced;
            stats_.rows_produced += page.row_count();
            return page;
        }
    }

    [[nodiscard]] ConnectorSplitStats Stats() const override { return stats_; }

    [[nodiscard]] bool Finished() const override { return stats_.finished; }

private:
    std::string bucket_;
    std::vector<std::shared_ptr<ObjectValue>> rows_;
    ScanRequest request_;
    size_t next_ = 0;
    size_t end_ = 0;
    size_t rows_per_page_ = 1024;
    std::vector<std::string> columns_;
    int64_t remaining_offset_ = std::max<int64_t>(0, request_.offset.value_or(0));
    std::optional<int64_t> remaining_limit_ =
        request_.limit.has_value() ? std::optional<int64_t>(std::max<int64_t>(0, *request_.limit))
                                   : std::nullopt;
    bool emitted_any_page_ = false;
    bool finished_ = false;
    ConnectorSplitStats stats_;
};

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
    return SourceCapabilities{
        .projection = true,
        .filter = true,
        .limit = true,
    };
}

absl::StatusOr<TableStatistics> MemoryConnectorMetadata::Statistics(
    const TableHandle& table) const {
    (void)table;
    return statistics_from_rows(rows_);
}

MemorySplitManager::MemorySplitManager(size_t row_count, size_t split_count)
    : row_count_(row_count), split_count_(std::max<size_t>(1, split_count)) {}

absl::StatusOr<std::vector<ConnectorSplit>> MemorySplitManager::GetSplits(
    const TableHandle& table, const ScanRequest& request) const {
    if (has_unsupported_memory_pushdown(request)) {
        return absl::UnimplementedError("memory source pushdown supports projection/filter/limit");
    }
    const size_t effective_split_count =
        request.limit.has_value() ? 1 : std::min(split_count_, std::max<size_t>(1, row_count_));
    std::vector<ConnectorSplit> splits;
    splits.reserve(effective_split_count);
    for (size_t index = 0; index < effective_split_count; ++index) {
        const size_t start = row_count_ * index / effective_split_count;
        const size_t end = row_count_ * (index + 1) / effective_split_count;
        splits.push_back(ConnectorSplit{.table = table,
                                        .request = request,
                                        .split_id = static_cast<int64_t>(index),
                                        .partition = std::to_string(index),
                                        .row_offset = start,
                                        .row_limit = end - start});
    }
    if (splits.empty()) {
        splits.push_back(
            ConnectorSplit{.table = table, .request = request, .split_id = 0, .partition = "0"});
    }
    return splits;
}

MemoryPageSourceProvider::MemoryPageSourceProvider(std::string bucket,
                                                   std::vector<std::shared_ptr<ObjectValue>> rows,
                                                   size_t rows_per_page)
    : bucket_(std::move(bucket)),
      rows_(std::move(rows)),
      rows_per_page_(std::max<size_t>(1, rows_per_page)) {}

absl::StatusOr<std::unique_ptr<ConnectorPageSource>> MemoryPageSourceProvider::CreatePageSource(
    const ConnectorSplit& split) const {
    if (has_unsupported_memory_pushdown(split.request)) {
        return absl::UnimplementedError("memory source pushdown supports projection/filter/limit");
    }
    return std::unique_ptr<ConnectorPageSource>(new MemoryPageSource(
        bucket_, rows_, split.request, split.row_offset, split.row_limit.value_or(rows_.size()),
        rows_per_page_, split.split_id));
}

std::unique_ptr<ConnectorRuntime> MakeMemoryConnectorRuntime(
    const SourceSpec& spec,
    std::string bucket,
    std::vector<std::shared_ptr<ObjectValue>> rows,
    size_t rows_per_page,
    size_t split_count) {
    auto runtime = std::make_unique<ConnectorRuntime>();
    runtime->metadata = std::make_unique<MemoryConnectorMetadata>(spec, bucket, rows);
    runtime->split_manager = std::make_unique<MemorySplitManager>(rows.size(), split_count);
    runtime->page_source_provider = std::make_unique<MemoryPageSourceProvider>(
        std::move(bucket), std::move(rows), rows_per_page);
    return runtime;
}

} // namespace pl::flux::connector
