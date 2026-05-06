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
#include "cpp/pl/flux/runtime_builtin_package.h"
#include <memory>
#include <sqlite3.h>
#include <string>
#include <utility>
#include <vector>

namespace pl::flux::builtin {
namespace {

struct SqliteDbDeleter {
    void operator()(sqlite3* db) const {
        if (db != nullptr) {
            sqlite3_close(db);
        }
    }
};

struct SqliteStmtDeleter {
    void operator()(sqlite3_stmt* stmt) const {
        if (stmt != nullptr) {
            sqlite3_finalize(stmt);
        }
    }
};

using SqliteDb = std::unique_ptr<sqlite3, SqliteDbDeleter>;
using SqliteStmt = std::unique_ptr<sqlite3_stmt, SqliteStmtDeleter>;

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

Value value_from_sqlite_column(sqlite3_stmt* stmt, int column) {
    switch (sqlite3_column_type(stmt, column)) {
        case SQLITE_NULL:
            return Value::null();
        case SQLITE_INTEGER:
            return Value::integer(sqlite3_column_int64(stmt, column));
        case SQLITE_FLOAT:
            return Value::floating(sqlite3_column_double(stmt, column));
        case SQLITE_BLOB: {
            const auto* bytes = static_cast<const char*>(sqlite3_column_blob(stmt, column));
            const int size = sqlite3_column_bytes(stmt, column);
            if (bytes == nullptr || size <= 0) {
                return Value::string("");
            }
            return Value::string(std::string(bytes, static_cast<size_t>(size)));
        }
        case SQLITE_TEXT:
        default: {
            const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, column));
            const int size = sqlite3_column_bytes(stmt, column);
            if (text == nullptr || size <= 0) {
                return Value::string("");
            }
            return Value::string(std::string(text, static_cast<size_t>(size)));
        }
    }
}

absl::StatusOr<Value> sqlite_query_to_table(const std::string& dsn, const std::string& query) {
    sqlite3* raw_db = nullptr;
    const int open_rc = sqlite3_open_v2(dsn.c_str(), &raw_db, SQLITE_OPEN_READONLY, nullptr);
    SqliteDb db(raw_db);
    if (open_rc != SQLITE_OK) {
        const char* message = raw_db == nullptr ? "unknown error" : sqlite3_errmsg(raw_db);
        return absl::InvalidArgumentError(absl::StrCat("sql.from sqlite open failed: ", message));
    }

    sqlite3_stmt* raw_stmt = nullptr;
    const int prepare_rc = sqlite3_prepare_v2(db.get(), query.c_str(),
                                              static_cast<int>(query.size()), &raw_stmt, nullptr);
    SqliteStmt stmt(raw_stmt);
    if (prepare_rc != SQLITE_OK) {
        return absl::InvalidArgumentError(
            absl::StrCat("sql.from sqlite prepare failed: ", sqlite3_errmsg(db.get())));
    }

    const int column_count = sqlite3_column_count(stmt.get());
    std::vector<std::string> column_names;
    column_names.reserve(static_cast<size_t>(column_count));
    for (int i = 0; i < column_count; ++i) {
        const char* name = sqlite3_column_name(stmt.get(), i);
        column_names.emplace_back(name == nullptr ? "" : name);
    }

    std::vector<std::shared_ptr<ObjectValue>> rows;
    while (true) {
        const int step_rc = sqlite3_step(stmt.get());
        if (step_rc == SQLITE_DONE) {
            break;
        }
        if (step_rc != SQLITE_ROW) {
            return absl::InvalidArgumentError(
                absl::StrCat("sql.from sqlite step failed: ", sqlite3_errmsg(db.get())));
        }

        std::vector<std::pair<std::string, Value>> properties;
        properties.reserve(column_names.size());
        for (int i = 0; i < column_count; ++i) {
            properties.emplace_back(column_names[static_cast<size_t>(i)],
                                    value_from_sqlite_column(stmt.get(), i));
        }
        rows.push_back(std::make_shared<ObjectValue>(std::move(properties)));
    }

    return Value::table("sql", std::move(rows));
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
    auto query_or = string_property(**object_or, "sql.from", "query");
    if (!query_or.ok()) {
        return query_or.status();
    }

    if (*driver_or != "sqlite") {
        return absl::UnimplementedError(absl::StrCat("sql.from unsupported driver: ", *driver_or));
    }
    return sqlite_query_to_table(*dsn_or, *query_or);
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
