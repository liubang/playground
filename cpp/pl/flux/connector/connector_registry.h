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
// Created: 2026/05/10 15:30

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "absl/status/statusor.h"

namespace pl::flux::connector {

struct ConnectorRuntime;

/// SourceSpec identifies a data source for connector creation.
/// It corresponds to plan::SourceScanSpec but lives in the connector layer
/// to avoid a circular dependency on plan/.
struct SourceSpec {
    std::string source{};
    std::string driver{};
    std::string dsn{};
    std::string table{};
};

/// Factory function type: given a SourceSpec, produce a complete connector runtime.
using ConnectorRuntimeFactory = std::function<std::unique_ptr<ConnectorRuntime>(const SourceSpec&)>;

/// ConnectorRegistry provides a centralized registry for connector factories.
/// New data sources register themselves at startup; the optimizer and executor
/// query the registry to create connector runtime instances without hard-coding
/// specific connector types.
class ConnectorRegistry {
public:
    /// Register a connector factory for the given source name.
    /// The source name should match SourceScanSpec.source (e.g., "sqlite", "mysql").
    void Register(std::string source, ConnectorRuntimeFactory factory);

    /// Create a connector runtime for the given spec. Returns an error if no
    /// factory is registered for the spec's source.
    [[nodiscard]] absl::StatusOr<std::unique_ptr<ConnectorRuntime>> CreateRuntime(
        const SourceSpec& spec) const;

    /// Check whether a connector is registered for the given source name.
    [[nodiscard]] bool HasConnector(const std::string& source) const;

    /// Return the list of registered source names (for diagnostics).
    [[nodiscard]] std::vector<std::string> RegisteredSources() const;

    /// Return the global singleton registry. Connectors register at static
    /// initialization time; all runtime code queries this instance.
    static ConnectorRegistry& Global();

private:
    std::unordered_map<std::string, ConnectorRuntimeFactory> factories_;
};

/// Helper: check if a source/driver pair is a pushdown-capable SQL connector.
/// This replaces the hard-coded checks scattered in rbo.cpp / physical_executor.cpp.
bool IsSqlPushdownConnector(const std::string& source, const std::string& driver);

/// Register all built-in connectors (sqlite, mysql, etc.) into the given registry.
/// Called once during global registry initialization.
void RegisterBuiltinConnectors(ConnectorRegistry& registry);

} // namespace pl::flux::connector
