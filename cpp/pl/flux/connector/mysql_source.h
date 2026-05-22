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
// Created: 2026/05/09

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "cpp/pl/flux/connector/connector_runtime.h"
#include "cpp/pl/flux/connector/table_source.h"
#include "cpp/pl/flux/runtime/runtime_value.h"

namespace pl::flux::connector {

struct MySQLConnectionConfig {
    std::string host;
    uint16_t port = 3306;
    std::string user;
    std::string password;
    std::string database;
    bool ssl = false;
};

absl::StatusOr<MySQLConnectionConfig> ParseMySQLDsn(const std::string& dsn);

class MySQLBoostConnectionPool;

struct MySQLRuntimeOptions {
    size_t target_split_count = 8;
    size_t rows_per_page = 1024;
    size_t max_pool_size = 8;
    size_t split_cache_max_entries = 1024;
    int64_t split_cache_ttl_ms = 300000;
    bool use_prepared_statements = true;
};

class MySQLSource final {
public:
    MySQLSource(std::string dsn, std::string table);
    MySQLSource(std::string dsn, std::string table, std::shared_ptr<MySQLBoostConnectionPool> pool);

    [[nodiscard]] absl::StatusOr<TableSchema> Schema() const;
    [[nodiscard]] SourceCapabilities Capabilities() const;
    [[nodiscard]] absl::StatusOr<TableStatistics> Statistics() const;
    absl::StatusOr<Value> Scan(const ScanRequest& request);

private:
    std::string dsn_;
    std::string table_;
    std::string query_;
    std::shared_ptr<MySQLBoostConnectionPool> pool_;
};

class MySQLConnectorMetadata final : public ConnectorMetadata {
public:
    explicit MySQLConnectorMetadata(SourceSpec spec);
    MySQLConnectorMetadata(SourceSpec spec, std::shared_ptr<MySQLBoostConnectionPool> pool);

    [[nodiscard]] absl::StatusOr<TableHandle> GetTableHandle(const SourceSpec& spec) const override;
    [[nodiscard]] absl::StatusOr<TableSchema> Schema(const TableHandle& table) const override;
    [[nodiscard]] SourceCapabilities Capabilities(const TableHandle& table) const override;
    [[nodiscard]] absl::StatusOr<TableStatistics> Statistics(
        const TableHandle& table) const override;

private:
    SourceSpec spec_;
    std::shared_ptr<MySQLBoostConnectionPool> pool_;
};

class MySQLSplitManager final : public ConnectorSplitManager {
public:
    explicit MySQLSplitManager(size_t target_split_count = 0);
    MySQLSplitManager(MySQLRuntimeOptions options, std::shared_ptr<MySQLBoostConnectionPool> pool);

    [[nodiscard]] absl::StatusOr<std::vector<ConnectorSplit>> GetSplits(
        const TableHandle& table, const ScanRequest& request) const override;

private:
    MySQLRuntimeOptions options_;
    std::shared_ptr<MySQLBoostConnectionPool> pool_;
    size_t target_split_count_ = 8;
};

class MySQLPageSourceProvider final : public ConnectorPageSourceProvider {
public:
    explicit MySQLPageSourceProvider(size_t rows_per_page = 1024);
    MySQLPageSourceProvider(MySQLRuntimeOptions options,
                            const std::shared_ptr<MySQLBoostConnectionPool>& pool);

    [[nodiscard]] absl::StatusOr<std::unique_ptr<ConnectorPageSource>> CreatePageSource(
        const ConnectorSplit& split) const override;

private:
    MySQLRuntimeOptions options_;
    size_t rows_per_page_ = 1024;
};

class MySQLPageSource final : public ConnectorPageSource {
public:
    MySQLPageSource(std::string dsn,
                    std::string table,
                    ScanRequest request,
                    size_t rows_per_page,
                    std::optional<std::string> split_column = std::nullopt,
                    std::optional<int64_t> split_lower = std::nullopt,
                    std::optional<int64_t> split_upper = std::nullopt,
                    int64_t split_id = 0,
                    MySQLRuntimeOptions options = {});

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
    MySQLRuntimeOptions options_;
    ConnectorSplitStats stats_;
};

std::unique_ptr<ConnectorRuntime> MakeMySQLConnectorRuntime(const SourceSpec& spec);

} // namespace pl::flux::connector
