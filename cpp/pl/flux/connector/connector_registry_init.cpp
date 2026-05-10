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

#include "cpp/pl/flux/connector/mysql_source.h"
#include "cpp/pl/flux/connector/sqlite_source.h"
#include <memory>

namespace pl::flux::connector {

void RegisterBuiltinConnectors(ConnectorRegistry& registry) {
    registry.Register("sqlite", [](const SourceSpec& spec) -> std::unique_ptr<TableSource> {
        return std::make_unique<SQLiteSource>(spec.dsn, spec.table);
    });

    registry.Register("mysql", [](const SourceSpec& spec) -> std::unique_ptr<TableSource> {
        return std::make_unique<MySQLSource>(spec.dsn, spec.table);
    });
}

} // namespace pl::flux::connector
