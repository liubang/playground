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

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/flux/plan/plan_node.h"
#include "cpp/pl/flux/runtime_builtin_package.h"
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pl::flux::builtin {
namespace {

Value make_builtin_value(const std::string& name, FunctionValue::BuiltinCallback fn) {
    auto callable = std::make_shared<FunctionValue>();
    callable->kind = FunctionValue::Kind::Builtin;
    callable->name = name;
    callable->builtin = std::move(fn);
    return Value::function(std::move(callable));
}

absl::StatusOr<const ObjectValue*> require_object_argument(const std::vector<Value>& args,
                                                           const std::string& name) {
    if (args.size() != 1 || args[0].type() != Value::Type::Object) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " expects exactly one object argument"));
    }
    return &args[0].as_object();
}

absl::StatusOr<std::string> string_property(const ObjectValue& object,
                                            const std::string& name,
                                            const std::string& property) {
    const Value* value = object.lookup(property);
    if (value == nullptr) {
        return absl::InvalidArgumentError(absl::StrCat(name, " requires `", property, "`"));
    }
    if (value->type() != Value::Type::String) {
        return absl::InvalidArgumentError(absl::StrCat(name, " `", property, "` must be a string"));
    }
    return value->as_string();
}

absl::Status reject_raw_query_property(const ObjectValue& object) {
    if (object.lookup("query") != nullptr) {
        return absl::InvalidArgumentError("sqlite.from does not accept `query`; use `table`");
    }
    if (object.lookup("driver") != nullptr || object.lookup("dsn") != nullptr) {
        return absl::InvalidArgumentError("sqlite.from expects `path` and `table`");
    }
    return absl::OkStatus();
}

absl::StatusOr<Value> builtin_sqlite_from(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "sqlite.from");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto query_status = reject_raw_query_property(**object_or);
    if (!query_status.ok()) {
        return query_status;
    }
    auto path_or = string_property(**object_or, "sqlite.from", "path");
    if (!path_or.ok()) {
        return path_or.status();
    }
    auto table_or = string_property(**object_or, "sqlite.from", "table");
    if (!table_or.ok()) {
        return table_or.status();
    }

    return Value::table_plan("sqlite", plan::MakeSourceScan("sqlite", "sqlite", *path_or, *table_or));
}

Value make_sqlite_package() {
    return Value::object({
        {"path", Value::string("sqlite")},
        {"from", make_builtin_value("sqlite.from", builtin_sqlite_from)},
    });
}

} // namespace

void RegisterSqliteStdlibPackage() { RegisterPackage("sqlite", make_sqlite_package); }

} // namespace pl::flux::builtin
