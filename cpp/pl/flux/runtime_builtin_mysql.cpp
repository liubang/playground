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
#include "cpp/pl/flux/connector/mysql_source.h"
#include "cpp/pl/flux/plan/plan_node.h"
#include "cpp/pl/flux/runtime_builtin_package.h"
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
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

absl::StatusOr<std::string> string_property(const ObjectValue& object,
                                            const std::string& name,
                                            const std::string& property) {
    auto status = require_string_property(object, name, property);
    if (!status.ok()) {
        return status;
    }
    return object.lookup(property)->as_string();
}

absl::StatusOr<std::optional<std::string>> optional_string_property(const ObjectValue& object,
                                                                    const std::string& name,
                                                                    const std::string& property) {
    const Value* value = object.lookup(property);
    if (value == nullptr) {
        return std::optional<std::string>{};
    }
    if (value->type() != Value::Type::String) {
        return absl::InvalidArgumentError(absl::StrCat(name, " `", property, "` must be a string"));
    }
    return std::optional<std::string>{value->as_string()};
}

absl::StatusOr<bool> optional_bool_property(const ObjectValue& object,
                                            const std::string& name,
                                            const std::string& property,
                                            bool default_value) {
    const Value* value = object.lookup(property);
    if (value == nullptr) {
        return default_value;
    }
    if (value->type() != Value::Type::Bool) {
        return absl::InvalidArgumentError(absl::StrCat(name, " `", property, "` must be a bool"));
    }
    return value->as_bool();
}

absl::StatusOr<uint16_t> optional_port_property(const ObjectValue& object,
                                                const std::string& name,
                                                uint16_t default_value) {
    const Value* value = object.lookup("port");
    if (value == nullptr) {
        return default_value;
    }
    uint64_t port = 0;
    if (value->type() == Value::Type::Int) {
        if (value->as_int() < 0) {
            return absl::InvalidArgumentError(absl::StrCat(name, " `port` must be a uint16"));
        }
        port = static_cast<uint64_t>(value->as_int());
    } else if (value->type() == Value::Type::UInt) {
        port = value->as_uint();
    } else {
        return absl::InvalidArgumentError(absl::StrCat(name, " `port` must be an int or uint"));
    }
    if (port > std::numeric_limits<uint16_t>::max()) {
        return absl::InvalidArgumentError(absl::StrCat(name, " `port` must be a uint16"));
    }
    return static_cast<uint16_t>(port);
}

absl::StatusOr<std::string> connection_dsn(const ObjectValue& object) {
    auto dsn_or = optional_string_property(object, "mysql.from", "dsn");
    if (!dsn_or.ok()) {
        return dsn_or.status();
    }
    if (dsn_or->has_value()) {
        if (object.lookup("host") != nullptr || object.lookup("user") != nullptr ||
            object.lookup("password") != nullptr || object.lookup("database") != nullptr ||
            object.lookup("port") != nullptr || object.lookup("ssl") != nullptr) {
            return absl::InvalidArgumentError(
                "mysql.from accepts either `dsn` or host/user/password/database/ssl, not both");
        }
        return **dsn_or;
    }

    auto host_or = string_property(object, "mysql.from", "host");
    if (!host_or.ok()) {
        return host_or.status();
    }
    auto user_or = string_property(object, "mysql.from", "user");
    if (!user_or.ok()) {
        return user_or.status();
    }
    auto password_or = string_property(object, "mysql.from", "password");
    if (!password_or.ok()) {
        return password_or.status();
    }
    auto database_or = string_property(object, "mysql.from", "database");
    if (!database_or.ok()) {
        return database_or.status();
    }
    auto port_or = optional_port_property(object, "mysql.from", 3306);
    if (!port_or.ok()) {
        return port_or.status();
    }
    auto ssl_or = optional_bool_property(object, "mysql.from", "ssl", false);
    if (!ssl_or.ok()) {
        return ssl_or.status();
    }

    return absl::StrCat("mysql://", *user_or, ":", *password_or, "@", *host_or, ":", *port_or, "/",
                        *database_or, "?ssl=", *ssl_or ? "true" : "false");
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
    auto table_or = string_property(**object_or, "mysql.from", "table");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto dsn_or = connection_dsn(**object_or);
    if (!dsn_or.ok()) {
        return dsn_or.status();
    }
    connector::MySQLSource source(*dsn_or, *table_or);
    auto value_or = source.Scan({});
    if (!value_or.ok()) {
        return absl::Status(value_or.status().code(),
                            absl::StrCat("mysql.from ", value_or.status().message()));
    }
    value_or->as_table_mut().plan = plan::MakeSourceScan("mysql", "mysql", *dsn_or, *table_or);
    return *value_or;
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
