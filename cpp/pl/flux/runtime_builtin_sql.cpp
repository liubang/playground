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
// Created: 2026/05/06 23:21

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/flux/connector/sqlite_source.h"
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
        return absl::InvalidArgumentError(
            "sql.from no longer accepts raw SQL `query`; use `table`");
    }
    return absl::OkStatus();
}

absl::StatusOr<Value> builtin_sql_from(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "sql.from");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto driver_or = string_property(**object_or, "sql.from", "driver");
    if (!driver_or.ok()) {
        return driver_or.status();
    }
    auto dsn_or = string_property(**object_or, "sql.from", "dsn");
    if (!dsn_or.ok()) {
        return dsn_or.status();
    }
    auto query_status = reject_raw_query_property(**object_or);
    if (!query_status.ok()) {
        return query_status;
    }
    auto table_or = string_property(**object_or, "sql.from", "table");
    if (!table_or.ok()) {
        return table_or.status();
    }

    if (*driver_or != "sqlite") {
        return absl::UnimplementedError(absl::StrCat("sql.from unsupported driver: ", *driver_or));
    }
    connector::SQLiteSource source(*dsn_or, *table_or);
    auto value_or = source.Scan({});
    if (!value_or.ok()) {
        return absl::Status(value_or.status().code(),
                            absl::StrCat("sql.from ", value_or.status().message()));
    }
    value_or->as_table_mut().plan = plan::MakeSourceScan("sql", *driver_or, *dsn_or, *table_or);
    return value_or;
}

Value make_sql_package() {
    return Value::object({
        {"path", Value::string("sql")},
        {"from", make_builtin_value("sql.from", builtin_sql_from)},
    });
}

} // namespace

void RegisterSqlStdlibPackage() { RegisterPackage("sql", make_sql_package); }

} // namespace pl::flux::builtin
