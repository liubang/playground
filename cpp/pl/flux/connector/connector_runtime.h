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
// Created: 2026/05/13 00:00

#pragma once

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "cpp/pl/flux/connector/connector_registry.h"
#include "cpp/pl/flux/connector/table_source.h"
#include "cpp/pl/flux/runtime/runtime_page.h"
#include "cpp/pl/flux/runtime/runtime_value.h"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pl::flux::connector {

struct TableHandle {
    std::string source;
    std::string driver;
    std::string dsn;
    std::string table;
};

struct ColumnHandle {
    std::string name;
    Value::Type type = Value::Type::Null;
    bool nullable = true;
};

struct ConnectorSplit {
    TableHandle table;
    ScanRequest request;
    int64_t split_id = 0;
    std::optional<std::string> partition;
    bool finished = false;
};

struct ConnectorSplitStats {
    int64_t split_id = 0;
    size_t pages_produced = 0;
    size_t rows_produced = 0;
    bool finished = false;
};

class ConnectorPageSource {
public:
    virtual ~ConnectorPageSource() = default;

    virtual absl::StatusOr<std::optional<Page>> NextPage() = 0;

    [[nodiscard]] virtual ConnectorSplitStats Stats() const { return {}; }

    [[nodiscard]] virtual bool Finished() const { return Stats().finished; }
};

class ConnectorMetadata {
public:
    virtual ~ConnectorMetadata() = default;

    [[nodiscard]] virtual absl::StatusOr<TableHandle> GetTableHandle(
        const SourceSpec& spec) const = 0;
    [[nodiscard]] virtual absl::StatusOr<TableSchema> Schema(const TableHandle& table) const = 0;
    [[nodiscard]] virtual SourceCapabilities Capabilities(const TableHandle& table) const = 0;
    [[nodiscard]] virtual absl::StatusOr<TableStatistics> Statistics(
        const TableHandle& table) const = 0;

    [[nodiscard]] virtual absl::StatusOr<ColumnHandle> GetColumnHandle(
        const TableHandle& table, const std::string& column) const {
        auto schema_or = Schema(table);
        if (!schema_or.ok()) {
            return schema_or.status();
        }
        for (const auto& item : schema_or->columns) {
            if (item.name == column) {
                return ColumnHandle{
                    .name = item.name,
                    .type = item.type,
                    .nullable = item.nullable,
                };
            }
        }
        return absl::InvalidArgumentError("unknown connector column: " + column);
    }
};

class ConnectorSplitManager {
public:
    virtual ~ConnectorSplitManager() = default;

    [[nodiscard]] virtual absl::StatusOr<std::vector<ConnectorSplit>> GetSplits(
        const TableHandle& table, const ScanRequest& request) const = 0;
};

class ConnectorPageSourceProvider {
public:
    virtual ~ConnectorPageSourceProvider() = default;

    [[nodiscard]] virtual absl::StatusOr<std::unique_ptr<ConnectorPageSource>> CreatePageSource(
        const ConnectorSplit& split) const = 0;
};

struct ConnectorRuntime {
    std::unique_ptr<ConnectorMetadata> metadata;
    std::unique_ptr<ConnectorSplitManager> split_manager;
    std::unique_ptr<ConnectorPageSourceProvider> page_source_provider;
};

class SingleSplitManager final : public ConnectorSplitManager {
public:
    [[nodiscard]] absl::StatusOr<std::vector<ConnectorSplit>> GetSplits(
        const TableHandle& table, const ScanRequest& request) const override {
        return std::vector<ConnectorSplit>{
            ConnectorSplit{.table = table, .request = request, .split_id = 0, .partition = {}}};
    }
};

class ChunkedPageSource final : public ConnectorPageSource {
public:
    ChunkedPageSource(std::string bucket,
                      std::vector<TableChunk> chunks,
                      size_t rows_per_page,
                      int64_t split_id = 0)
        : bucket_(std::move(bucket)),
          chunks_(std::move(chunks)),
          rows_per_page_(std::max<size_t>(1, rows_per_page)) {
        stats_.split_id = split_id;
    }

    absl::StatusOr<std::optional<Page>> NextPage() override {
        if (next_chunk_ >= chunks_.size()) {
            stats_.finished = true;
            return std::nullopt;
        }

        const TableChunk& source = chunks_[next_chunk_];
        TableChunk chunk;
        chunk.group_key = source.group_key;
        chunk.columns = source.columns;
        if (source.rows.empty()) {
            ++next_chunk_;
        } else {
            const size_t end = std::min(source.rows.size(), next_row_ + rows_per_page_);
            chunk.rows.insert(chunk.rows.end(),
                              source.rows.begin() + static_cast<std::ptrdiff_t>(next_row_),
                              source.rows.begin() + static_cast<std::ptrdiff_t>(end));
            if (end >= source.rows.size()) {
                ++next_chunk_;
                next_row_ = 0;
            } else {
                next_row_ = end;
            }
        }

        std::vector<TableChunk> chunks;
        chunks.push_back(std::move(chunk));
        Page page = PageFromTableChunks(bucket_, chunks);
        ++stats_.pages_produced;
        stats_.rows_produced += page.row_count();
        return page;
    }

    [[nodiscard]] ConnectorSplitStats Stats() const override { return stats_; }

    [[nodiscard]] bool Finished() const override { return stats_.finished; }

private:
    std::string bucket_;
    std::vector<TableChunk> chunks_;
    size_t rows_per_page_ = 1024;
    size_t next_chunk_ = 0;
    size_t next_row_ = 0;
    ConnectorSplitStats stats_;
};

} // namespace pl::flux::connector
