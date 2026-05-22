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

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "cpp/pl/flux/connector/connector_runtime.h"
#include "cpp/pl/flux/connector/table_source.h"
#include "cpp/pl/flux/runtime/runtime_value.h"

namespace pl::flux::connector {

class ArraySource final {
public:
    ArraySource(std::string bucket, std::vector<std::shared_ptr<ObjectValue>> rows);

    [[nodiscard]] absl::StatusOr<TableSchema> Schema() const;
    [[nodiscard]] SourceCapabilities Capabilities() const;
    [[nodiscard]] absl::StatusOr<TableStatistics> Statistics() const;
    absl::StatusOr<Value> Scan(const ScanRequest& request);

private:
    std::string bucket_;
    std::vector<std::shared_ptr<ObjectValue>> rows_;
};

class CsvSource final {
public:
    explicit CsvSource(std::vector<std::shared_ptr<ObjectValue>> rows);

    [[nodiscard]] absl::StatusOr<TableSchema> Schema() const;
    [[nodiscard]] SourceCapabilities Capabilities() const;
    [[nodiscard]] absl::StatusOr<TableStatistics> Statistics() const;
    absl::StatusOr<Value> Scan(const ScanRequest& request);

private:
    std::vector<std::shared_ptr<ObjectValue>> rows_;
};

class MemoryConnectorMetadata final : public ConnectorMetadata {
public:
    MemoryConnectorMetadata(SourceSpec spec,
                            std::string bucket,
                            std::vector<std::shared_ptr<ObjectValue>> rows);

    [[nodiscard]] absl::StatusOr<TableHandle> GetTableHandle(const SourceSpec& spec) const override;
    [[nodiscard]] absl::StatusOr<TableSchema> Schema(const TableHandle& table) const override;
    [[nodiscard]] SourceCapabilities Capabilities(const TableHandle& table) const override;
    [[nodiscard]] absl::StatusOr<TableStatistics> Statistics(
        const TableHandle& table) const override;

private:
    SourceSpec spec_;
    std::string bucket_;
    std::vector<std::shared_ptr<ObjectValue>> rows_;
};

class MemorySplitManager final : public ConnectorSplitManager {
public:
    MemorySplitManager(size_t row_count, size_t split_count);

    [[nodiscard]] absl::StatusOr<std::vector<ConnectorSplit>> GetSplits(
        const TableHandle& table, const ScanRequest& request) const override;

private:
    size_t row_count_ = 0;
    size_t split_count_ = 1;
};

class MemoryPageSourceProvider final : public ConnectorPageSourceProvider {
public:
    MemoryPageSourceProvider(std::string bucket,
                             std::vector<std::shared_ptr<ObjectValue>> rows,
                             size_t rows_per_page = 1024);

    [[nodiscard]] absl::StatusOr<std::unique_ptr<ConnectorPageSource>> CreatePageSource(
        const ConnectorSplit& split) const override;

private:
    std::string bucket_;
    std::vector<std::shared_ptr<ObjectValue>> rows_;
    size_t rows_per_page_ = 1024;
};

std::unique_ptr<ConnectorRuntime> MakeMemoryConnectorRuntime(
    const SourceSpec& spec,
    std::string bucket,
    std::vector<std::shared_ptr<ObjectValue>> rows,
    size_t rows_per_page = 1024,
    size_t split_count = 1);

} // namespace pl::flux::connector
