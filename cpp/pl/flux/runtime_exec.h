// Copyright (c) 2023 The Authors. All rights reserved.
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

#pragma once

#include "absl/status/statusor.h"
#include "cpp/pl/flux/ast.h"
#include "cpp/pl/flux/runtime_env.h"

namespace pl {

struct ExecutionResult {
    enum class Type {
        Normal,
        Return,
    };

    Type type = Type::Normal;
    Value value = Value::null();

    static ExecutionResult normal(Value value = Value::null()) {
        return ExecutionResult{.type = Type::Normal, .value = std::move(value)};
    }

    static ExecutionResult returned(Value value) {
        return ExecutionResult{.type = Type::Return, .value = std::move(value)};
    }
};

struct NamedResult {
    std::string name;
    Value value = Value::null();
};

struct FileExecutionResult {
    ExecutionResult last = ExecutionResult::normal();
    std::string package_name;
    std::vector<std::string> imports;
    std::vector<NamedResult> results;
};

class StatementExecutor {
public:
    static absl::StatusOr<ExecutionResult> Execute(const Statement& stmt, Environment& env);
    static absl::StatusOr<ExecutionResult> ExecuteBlock(const Block& block, Environment& env);
    static absl::StatusOr<FileExecutionResult> ExecuteFile(const File& file, Environment& env);
};

} // namespace pl
