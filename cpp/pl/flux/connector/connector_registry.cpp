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

#include "cpp/pl/flux/connector/connector_registry.h"

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

namespace pl::flux::connector {

void ConnectorRegistry::Register(std::string source, ConnectorFactory factory) {
    factories_[std::move(source)] = std::move(factory);
}

absl::StatusOr<std::unique_ptr<TableSource>> ConnectorRegistry::Create(
    const SourceSpec& spec) const {
    auto it = factories_.find(spec.source);
    if (it == factories_.end()) {
        return absl::InvalidArgumentError(
            absl::StrCat("no connector registered for source: ", spec.source));
    }
    auto source = it->second(spec);
    if (source == nullptr) {
        return absl::InternalError(
            absl::StrCat("connector factory returned null for source: ", spec.source));
    }
    return source;
}

bool ConnectorRegistry::HasConnector(const std::string& source) const {
    return factories_.find(source) != factories_.end();
}

std::vector<std::string> ConnectorRegistry::RegisteredSources() const {
    std::vector<std::string> sources;
    sources.reserve(factories_.size());
    for (const auto& [name, _] : factories_) {
        sources.push_back(name);
    }
    return sources;
}

ConnectorRegistry& ConnectorRegistry::Global() {
    static ConnectorRegistry instance = [] {
        ConnectorRegistry r;
        RegisterBuiltinConnectors(r);
        return r;
    }();
    return instance;
}

bool IsSqlPushdownConnector(const std::string& source, const std::string& driver) {
    // A source is a pushdown-capable SQL connector if it is registered in the
    // global registry and its driver matches its source name (convention for
    // SQL-based connectors).
    if (!ConnectorRegistry::Global().HasConnector(source)) {
        return false;
    }
    return source == driver;
}

} // namespace pl::flux::connector
