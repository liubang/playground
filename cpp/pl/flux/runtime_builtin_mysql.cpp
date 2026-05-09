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

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
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

absl::Status require_string_property(const ObjectValue& object,
                                     const std::string& name,
                                     const std::string& property) {
    const Value* value = object.lookup(property);
    if (value == nullptr) {
        return absl::InvalidArgumentError(absl::StrCat(name, " requires `", property, "`"));
    }
    if (value->type() != Value::Type::String) {
        return absl::InvalidArgumentError(absl::StrCat(name, " `", property, "` must be a string"));
    }
    return absl::OkStatus();
}

absl::Status reject_raw_query_property(const ObjectValue& object) {
    if (object.lookup("query") != nullptr) {
        return absl::InvalidArgumentError("mysql.from does not accept `query`; use `table`");
    }
    return absl::OkStatus();
}

absl::StatusOr<Value> builtin_mysql_from(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "mysql.from");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto query_status = reject_raw_query_property(**object_or);
    if (!query_status.ok()) {
        return query_status;
    }
    auto dsn_status = require_string_property(**object_or, "mysql.from", "dsn");
    if (!dsn_status.ok()) {
        return dsn_status;
    }
    auto table_status = require_string_property(**object_or, "mysql.from", "table");
    if (!table_status.ok()) {
        return table_status;
    }
    return absl::UnimplementedError("mysql.from connector is not implemented yet");
}

Value make_mysql_package() {
    return Value::object({
        {"path", Value::string("mysql")},
        {"from", make_builtin_value("mysql.from", builtin_mysql_from)},
    });
}

} // namespace

void RegisterMysqlStdlibPackage() { RegisterPackage("mysql", make_mysql_package); }

} // namespace pl::flux::builtin
