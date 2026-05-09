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
#include "cpp/pl/flux/connector/table_source.h"
#include "cpp/pl/flux/runtime_value.h"
#include <string>

namespace pl::flux::connector {

class SQLiteSource final : public TableSource {
public:
    SQLiteSource(std::string dsn, std::string table);

    [[nodiscard]] absl::StatusOr<TableSchema> Schema() const override;
    [[nodiscard]] SourceCapabilities Capabilities() const override;
    absl::StatusOr<Value> Scan(const ScanRequest& request) override;

private:
    std::string dsn_;
    std::string table_;
    std::string query_;
};

} // namespace pl::flux::connector
