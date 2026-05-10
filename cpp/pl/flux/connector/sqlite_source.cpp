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

#include "cpp/pl/flux/connector/sqlite_source.h"

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/flux/connector/sql_builder.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <sqlite3.h>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pl::flux::connector {
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

absl::StatusOr<SqliteDb> open_readonly_db(const std::string& dsn) {
    sqlite3* raw_db = nullptr;
    const int open_rc = sqlite3_open_v2(dsn.c_str(), &raw_db, SQLITE_OPEN_READONLY, nullptr);
    SqliteDb db(raw_db);
    if (open_rc != SQLITE_OK) {
        const char* message = raw_db == nullptr ? "unknown error" : sqlite3_errmsg(raw_db);
        return absl::InvalidArgumentError(absl::StrCat("sqlite open failed: ", message));
    }
    return db;
}

absl::StatusOr<SqliteStmt> prepare_statement(sqlite3* db, const std::string& query) {
    sqlite3_stmt* raw_stmt = nullptr;
    const int prepare_rc =
        sqlite3_prepare_v2(db, query.c_str(), static_cast<int>(query.size()), &raw_stmt, nullptr);
    SqliteStmt stmt(raw_stmt);
    if (prepare_rc != SQLITE_OK) {
        return absl::InvalidArgumentError(
            absl::StrCat("sqlite prepare failed: ", sqlite3_errmsg(db)));
    }
    return stmt;
}

std::string quote_identifier(const std::string& identifier) {
    std::string out = "\"";
    for (const char ch : identifier) {
        if (ch == '"') {
            out += "\"\"";
        } else {
            out.push_back(ch);
        }
    }
    out.push_back('"');
    return out;
}

// predicate_op_sql / aggregate_fn_sql / validate_column are now shared via
// sql_builder.h as PredicateOpSql / AggregateFnSql / ValidateColumn.

struct SqliteParam {
    Value value;
};

absl::Status bind_value(sqlite3_stmt* stmt, int index, const Value& value) {
    int rc = SQLITE_OK;
    switch (value.type()) {
        case Value::Type::Null:
            rc = sqlite3_bind_null(stmt, index);
            break;
        case Value::Type::Bool:
            rc = sqlite3_bind_int64(stmt, index, value.as_bool() ? 1 : 0);
            break;
        case Value::Type::Int:
            rc = sqlite3_bind_int64(stmt, index, value.as_int());
            break;
        case Value::Type::UInt:
            if (value.as_uint() > static_cast<uint64_t>(INT64_MAX)) {
                return absl::InvalidArgumentError("sqlite source uint parameter overflows int64");
            }
            rc = sqlite3_bind_int64(stmt, index, static_cast<int64_t>(value.as_uint()));
            break;
        case Value::Type::Float:
            rc = sqlite3_bind_double(stmt, index, value.as_float());
            break;
        case Value::Type::String:
            rc = sqlite3_bind_text(stmt, index, value.as_string().c_str(),
                                   static_cast<int>(value.as_string().size()), SQLITE_TRANSIENT);
            break;
        case Value::Type::Time:
            rc = sqlite3_bind_text(stmt, index, value.as_time().literal.c_str(),
                                   static_cast<int>(value.as_time().literal.size()),
                                   SQLITE_TRANSIENT);
            break;
        default:
            return absl::InvalidArgumentError("sqlite source parameter type is not pushable");
    }
    if (rc != SQLITE_OK) {
        return absl::InvalidArgumentError("sqlite bind failed");
    }
    return absl::OkStatus();
}

absl::Status bind_int64(sqlite3_stmt* stmt, int index, int64_t value) {
    const int rc = sqlite3_bind_int64(stmt, index, value);
    if (rc != SQLITE_OK) {
        return absl::InvalidArgumentError("sqlite bind failed");
    }
    return absl::OkStatus();
}

struct BuiltSql {
    std::string sql;
    std::vector<SqliteParam> params;
    std::optional<int64_t> limit;
    std::optional<int64_t> offset;
};

absl::StatusOr<BuiltSql> build_scan_sql(const std::string& query,
                                        const ScanRequest& request,
                                        const TableSchema& schema) {
    std::unordered_set<std::string> schema_columns;
    schema_columns.reserve(schema.columns.size());
    for (const auto& column : schema.columns) {
        schema_columns.insert(column.name);
    }

    if (request.distinct.has_value()) {
        auto status = ValidateColumn(schema_columns, *request.distinct, "sqlite", "distinct");
        if (!status.ok()) {
            return status;
        }
    }

    std::string sql = "SELECT ";
    if (request.aggregate.has_value()) {
        for (size_t i = 0; i < request.group_by.size(); ++i) {
            auto status = ValidateColumn(schema_columns, request.group_by[i], "sqlite", "group");
            if (!status.ok()) {
                return status;
            }
            if (i != 0) {
                sql += ", ";
            }
            sql += quote_identifier(request.group_by[i]);
        }
        if (!request.group_by.empty()) {
            sql += ", ";
        }
        auto status =
            ValidateColumn(schema_columns, request.aggregate->column, "sqlite", "aggregate");
        if (!status.ok()) {
            return status;
        }
        sql += AggregateFnSql(request.aggregate->fn);
        sql += "(";
        sql += quote_identifier(request.aggregate->column);
        sql += ") AS ";
        sql += quote_identifier(request.aggregate->alias.empty() ? request.aggregate->column
                                                                 : request.aggregate->alias);
    } else if (!request.projection_columns.empty()) {
        for (size_t i = 0; i < request.projection_columns.size(); ++i) {
            const auto& projection = request.projection_columns[i];
            auto status = ValidateColumn(schema_columns, projection.column, "sqlite", "projection");
            if (!status.ok()) {
                return status;
            }
            if (i != 0) {
                sql += ", ";
            }
            sql += quote_identifier(projection.column);
            if (!projection.alias.empty() && projection.alias != projection.column) {
                sql += " AS ";
                sql += quote_identifier(projection.alias);
            }
        }
    } else if (request.columns.empty()) {
        sql += "*";
    } else {
        for (size_t i = 0; i < request.columns.size(); ++i) {
            auto status =
                ValidateColumn(schema_columns, request.columns[i], "sqlite", "projection");
            if (!status.ok()) {
                return status;
            }
            if (i != 0) {
                sql += ", ";
            }
            sql += quote_identifier(request.columns[i]);
        }
    }
    sql += " FROM (";
    sql += query;
    sql += ") AS flux_source";

    std::vector<std::string> where_clauses;
    std::vector<SqliteParam> params;
    if (request.time_range.has_value()) {
        auto status = ValidateColumn(schema_columns, "_time", "sqlite", "time range");
        if (!status.ok()) {
            return status;
        }
        if (request.time_range->start.has_value()) {
            where_clauses.push_back(quote_identifier("_time") + " >= ?");
            params.push_back({Value::string(*request.time_range->start)});
        }
        if (request.time_range->stop.has_value()) {
            where_clauses.push_back(quote_identifier("_time") + " < ?");
            params.push_back({Value::string(*request.time_range->stop)});
        }
    }
    for (const auto& predicate : request.predicates) {
        auto status = ValidateColumn(schema_columns, predicate.column, "sqlite", "predicate");
        if (!status.ok()) {
            return status;
        }
        where_clauses.push_back(quote_identifier(predicate.column) + " " +
                                PredicateOpSql(predicate.op) + " ?");
        params.push_back({predicate.literal});
    }
    if (!where_clauses.empty()) {
        sql += " WHERE ";
        for (size_t i = 0; i < where_clauses.size(); ++i) {
            if (i != 0) {
                sql += " AND ";
            }
            sql += where_clauses[i];
        }
    }

    if (request.aggregate.has_value() && !request.group_by.empty()) {
        sql += " GROUP BY ";
        for (size_t i = 0; i < request.group_by.size(); ++i) {
            if (i != 0) {
                sql += ", ";
            }
            sql += quote_identifier(request.group_by[i]);
        }
    } else if (request.distinct.has_value()) {
        sql += " GROUP BY ";
        sql += quote_identifier(*request.distinct);
    }

    if (!request.order_by.empty()) {
        sql += " ORDER BY ";
        for (size_t i = 0; i < request.order_by.size(); ++i) {
            auto status =
                ValidateColumn(schema_columns, request.order_by[i].column, "sqlite", "sort");
            if (!status.ok()) {
                return status;
            }
            if (i != 0) {
                sql += ", ";
            }
            sql += quote_identifier(request.order_by[i].column);
            sql += request.order_by[i].desc ? " DESC" : " ASC";
        }
    }
    if (request.limit.has_value()) {
        if (*request.limit < 0) {
            return absl::InvalidArgumentError("sqlite source limit must be non-negative");
        }
        sql += " LIMIT ?";
    }
    if (request.offset.has_value()) {
        if (*request.offset < 0) {
            return absl::InvalidArgumentError("sqlite source offset must be non-negative");
        }
        if (!request.limit.has_value()) {
            sql += " LIMIT -1";
        }
        sql += " OFFSET ?";
    }

    return BuiltSql{.sql = std::move(sql),
                    .params = std::move(params),
                    .limit = request.limit,
                    .offset = request.offset};
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

} // namespace

SQLiteSource::SQLiteSource(std::string dsn, std::string table)
    : dsn_(std::move(dsn)),
      table_(std::move(table)),
      query_(absl::StrCat("SELECT * FROM ", quote_identifier(table_))) {}

absl::StatusOr<TableSchema> SQLiteSource::Schema() const {
    if (cached_schema_.has_value()) {
        return *cached_schema_;
    }

    auto db_or = open_readonly_db(dsn_);
    if (!db_or.ok()) {
        return db_or.status();
    }
    auto stmt_or = prepare_statement(db_or->get(), query_);
    if (!stmt_or.ok()) {
        return stmt_or.status();
    }

    TableSchema schema;
    const int column_count = sqlite3_column_count(stmt_or->get());
    schema.columns.reserve(static_cast<size_t>(column_count));
    for (int i = 0; i < column_count; ++i) {
        const char* name = sqlite3_column_name(stmt_or->get(), i);
        schema.columns.push_back(
            {.name = name == nullptr ? "" : name, .type = Value::Type::Null, .nullable = true});
    }
    cached_schema_ = schema;
    return schema;
}

SourceCapabilities SQLiteSource::Capabilities() const {
    return SourceCapabilities{
        .projection = true,
        .filter = true,
        .time_range = true,
        .limit = true,
        .sort = true,
        .aggregate = true,
        .distinct = true,
    };
}

absl::StatusOr<TableStatistics> SQLiteSource::Statistics() const {
    if (cached_statistics_.has_value()) {
        return *cached_statistics_;
    }

    auto db_or = open_readonly_db(dsn_);
    if (!db_or.ok()) {
        return db_or.status();
    }
    auto stmt_or = prepare_statement(
        db_or->get(), absl::StrCat("SELECT COUNT(*) FROM (", query_, ") AS flux_source"));
    if (!stmt_or.ok()) {
        return stmt_or.status();
    }

    TableStatistics statistics;
    const int step_rc = sqlite3_step(stmt_or->get());
    if (step_rc != SQLITE_ROW) {
        if (step_rc == SQLITE_DONE) {
            statistics.row_count = 0.0;
        } else {
            return absl::InvalidArgumentError(
                absl::StrCat("sqlite statistics failed: ", sqlite3_errmsg(db_or->get())));
        }
    } else {
        statistics.row_count = static_cast<double>(sqlite3_column_int64(stmt_or->get(), 0));
    }

    auto schema_or = Schema();
    if (schema_or.ok()) {
        statistics.columns.reserve(schema_or->columns.size());
        for (const auto& column : schema_or->columns) {
            statistics.columns.push_back({.name = column.name});
        }
    }

    cached_statistics_ = statistics;
    return statistics;
}

absl::StatusOr<Value> SQLiteSource::Scan(const ScanRequest& request) {
    auto db_or = open_readonly_db(dsn_);
    if (!db_or.ok()) {
        return db_or.status();
    }
    auto schema_or = Schema();
    if (!schema_or.ok()) {
        return schema_or.status();
    }
    auto sql_or = build_scan_sql(query_, request, *schema_or);
    if (!sql_or.ok()) {
        return sql_or.status();
    }

    auto stmt_or = prepare_statement(db_or->get(), sql_or->sql);
    if (!stmt_or.ok()) {
        return stmt_or.status();
    }

    sqlite3_stmt* stmt = stmt_or->get();
    int bind_index = 1;
    for (const auto& param : sql_or->params) {
        auto status = bind_value(stmt, bind_index, param.value);
        if (!status.ok()) {
            return status;
        }
        ++bind_index;
    }
    if (sql_or->limit.has_value()) {
        auto status = bind_int64(stmt, bind_index, *sql_or->limit);
        if (!status.ok()) {
            return status;
        }
        ++bind_index;
    }
    if (sql_or->offset.has_value()) {
        auto status = bind_int64(stmt, bind_index, *sql_or->offset);
        if (!status.ok()) {
            return status;
        }
        ++bind_index;
    }

    const int column_count = sqlite3_column_count(stmt);
    std::vector<std::string> column_names;
    column_names.reserve(static_cast<size_t>(column_count));
    for (int i = 0; i < column_count; ++i) {
        const char* name = sqlite3_column_name(stmt, i);
        column_names.emplace_back(name == nullptr ? "" : name);
    }

    std::vector<std::shared_ptr<ObjectValue>> rows;
    while (true) {
        const int step_rc = sqlite3_step(stmt);
        if (step_rc == SQLITE_DONE) {
            break;
        }
        if (step_rc != SQLITE_ROW) {
            return absl::InvalidArgumentError(
                absl::StrCat("sqlite step failed: ", sqlite3_errmsg(db_or->get())));
        }

        std::vector<std::pair<std::string, Value>> properties;
        properties.reserve(column_names.size());
        for (int i = 0; i < column_count; ++i) {
            properties.emplace_back(column_names[static_cast<size_t>(i)],
                                    value_from_sqlite_column(stmt, i));
        }
        rows.push_back(std::make_shared<ObjectValue>(std::move(properties)));
    }

    if (request.aggregate.has_value()) {
        std::vector<TableChunk> chunks;
        chunks.reserve(rows.size());
        for (auto& row : rows) {
            std::vector<std::pair<std::string, Value>> group_props;
            group_props.reserve(request.group_by.size());
            for (const auto& column : request.group_by) {
                const Value* value = row == nullptr ? nullptr : row->lookup(column);
                if (value != nullptr) {
                    group_props.emplace_back(column, *value);
                }
            }
            std::vector<std::pair<std::string, Value>> props = row->properties;
            props.emplace_back("_group", Value::object(std::move(group_props)));
            row = std::make_shared<ObjectValue>(std::move(props));
            TableChunk chunk;
            chunk.rows.push_back(row);
            chunks.push_back(std::move(chunk));
        }
        return Value::table_stream("sqlite", std::move(chunks));
    }

    return Value::table("sqlite", std::move(rows));
}

} // namespace pl::flux::connector
