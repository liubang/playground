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
#include "cpp/pl/flux/connector/connector_runtime.h"
#include "cpp/pl/flux/connector/table_source.h"
#include "cpp/pl/flux/runtime/runtime_value.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pl::flux::connector {

class SQLiteSource final {
public:
    SQLiteSource(std::string dsn, std::string table);

    [[nodiscard]] absl::StatusOr<TableSchema> Schema() const;
    [[nodiscard]] SourceCapabilities Capabilities() const;
    [[nodiscard]] absl::StatusOr<TableStatistics> Statistics() const;
    absl::StatusOr<Value> Scan(const ScanRequest& request);

private:
    std::string dsn_;
    std::string table_;
    std::string query_;
    mutable std::optional<TableSchema> cached_schema_;
    mutable std::optional<TableStatistics> cached_statistics_;
};

class SQLiteConnectorMetadata final : public ConnectorMetadata {
public:
    explicit SQLiteConnectorMetadata(SourceSpec spec);

    [[nodiscard]] absl::StatusOr<TableHandle> GetTableHandle(const SourceSpec& spec) const override;
    [[nodiscard]] absl::StatusOr<TableSchema> Schema(const TableHandle& table) const override;
    [[nodiscard]] SourceCapabilities Capabilities(const TableHandle& table) const override;
    [[nodiscard]] absl::StatusOr<TableStatistics> Statistics(
        const TableHandle& table) const override;

private:
    SourceSpec spec_;
};

class SQLiteSplitManager final : public ConnectorSplitManager {
public:
    explicit SQLiteSplitManager(size_t target_split_count = 0);

    [[nodiscard]] absl::StatusOr<std::vector<ConnectorSplit>> GetSplits(
        const TableHandle& table, const ScanRequest& request) const override;

private:
    size_t target_split_count_ = 0;
};

class SQLitePageSource final : public ConnectorPageSource {
public:
    SQLitePageSource(std::string dsn,
                     std::string table,
                     ScanRequest request,
                     size_t rows_per_page,
                     std::optional<int64_t> rowid_lower = std::nullopt,
                     std::optional<int64_t> rowid_upper = std::nullopt,
                     int64_t split_id = 0);

    absl::Status Initialize();
    absl::StatusOr<std::optional<Page>> NextPage() override;
    [[nodiscard]] ConnectorSplitStats Stats() const override;
    [[nodiscard]] bool Finished() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string dsn_;
    std::string table_;
    size_t rows_per_page_ = 1024;
    ConnectorSplitStats stats_;
};

class SQLitePageSourceProvider final : public ConnectorPageSourceProvider {
public:
    explicit SQLitePageSourceProvider(size_t rows_per_page = 1024);

    [[nodiscard]] absl::StatusOr<std::unique_ptr<ConnectorPageSource>> CreatePageSource(
        const ConnectorSplit& split) const override;

private:
    size_t rows_per_page_ = 1024;
};

std::unique_ptr<ConnectorRuntime> MakeSQLiteConnectorRuntime(const SourceSpec& spec);

} // namespace pl::flux::connector
