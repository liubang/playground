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
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <sqlite3.h>
#include <string>
#include <thread>
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

std::string query_for_table(const std::string& table) {
    return absl::StrCat("SELECT * FROM ", quote_identifier(table));
}

std::string query_for_table_rowid_range(const std::string& table, int64_t lower, int64_t upper) {
    return absl::StrCat("SELECT * FROM ", quote_identifier(table), " WHERE rowid >= ", lower,
                        " AND rowid <= ", upper);
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

bool request_requires_global_sql_order(const ScanRequest& request) {
    return !request.order_by.empty() || request.limit.has_value() || request.offset.has_value() ||
           !request.group_by.empty() || request.aggregate.has_value() ||
           request.distinct.has_value();
}

struct RowidExtent {
    int64_t min_rowid = 0;
    int64_t max_rowid = -1;
    size_t row_count = 0;
};

absl::StatusOr<RowidExtent> load_rowid_extent(sqlite3* db, const std::string& table) {
    auto stmt_or = prepare_statement(
        db, absl::StrCat("SELECT MIN(rowid), MAX(rowid), COUNT(*) FROM ", quote_identifier(table)));
    if (!stmt_or.ok()) {
        return stmt_or.status();
    }
    const int step_rc = sqlite3_step(stmt_or->get());
    if (step_rc != SQLITE_ROW) {
        return absl::InvalidArgumentError(
            absl::StrCat("sqlite rowid split discovery failed: ", sqlite3_errmsg(db)));
    }
    if (sqlite3_column_type(stmt_or->get(), 0) == SQLITE_NULL ||
        sqlite3_column_type(stmt_or->get(), 1) == SQLITE_NULL) {
        return RowidExtent{};
    }
    return RowidExtent{
        .min_rowid = sqlite3_column_int64(stmt_or->get(), 0),
        .max_rowid = sqlite3_column_int64(stmt_or->get(), 1),
        .row_count = static_cast<size_t>(sqlite3_column_int64(stmt_or->get(), 2)),
    };
}

size_t default_sqlite_split_count() {
    const unsigned int workers = std::thread::hardware_concurrency();
    if (workers == 0) {
        return 4;
    }
    return std::min<size_t>(8, std::max<size_t>(1, workers));
}

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

absl::Status bind_scan_sql(sqlite3_stmt* stmt, const BuiltSql& sql) {
    int bind_index = 1;
    for (const auto& param : sql.params) {
        auto status = bind_value(stmt, bind_index, param.value);
        if (!status.ok()) {
            return status;
        }
        ++bind_index;
    }
    if (sql.limit.has_value()) {
        auto status = bind_int64(stmt, bind_index, *sql.limit);
        if (!status.ok()) {
            return status;
        }
        ++bind_index;
    }
    if (sql.offset.has_value()) {
        auto status = bind_int64(stmt, bind_index, *sql.offset);
        if (!status.ok()) {
            return status;
        }
    }
    return absl::OkStatus();
}

std::vector<std::string> sqlite_column_names(sqlite3_stmt* stmt) {
    const int column_count = sqlite3_column_count(stmt);
    std::vector<std::string> column_names;
    column_names.reserve(static_cast<size_t>(column_count));
    for (int i = 0; i < column_count; ++i) {
        const char* name = sqlite3_column_name(stmt, i);
        column_names.emplace_back(name == nullptr ? "" : name);
    }
    return column_names;
}

std::shared_ptr<ObjectValue> row_from_sqlite_stmt(sqlite3_stmt* stmt,
                                                  const std::vector<std::string>& column_names) {
    std::vector<std::pair<std::string, Value>> properties;
    properties.reserve(column_names.size());
    for (int i = 0; i < static_cast<int>(column_names.size()); ++i) {
        properties.emplace_back(column_names[static_cast<size_t>(i)],
                                value_from_sqlite_column(stmt, i));
    }
    return std::make_shared<ObjectValue>(std::move(properties));
}

PageChunk empty_page_chunk_for_columns(const std::vector<std::string>& column_names) {
    PageChunk chunk;
    chunk.columns.reserve(column_names.size());
    for (const auto& name : column_names) {
        chunk.columns.push_back(ColumnVector{.name = name});
    }
    return chunk;
}

void append_sqlite_stmt_to_page_chunk(sqlite3_stmt* stmt, PageChunk* chunk) {
    if (chunk == nullptr) {
        return;
    }
    if (chunk->columns.empty()) {
        const int column_count = sqlite3_column_count(stmt);
        chunk->columns.reserve(static_cast<size_t>(column_count));
        for (int i = 0; i < column_count; ++i) {
            const char* name = sqlite3_column_name(stmt, i);
            chunk->columns.push_back(ColumnVector{.name = name == nullptr ? "" : name});
        }
    }
    for (int i = 0; i < static_cast<int>(chunk->columns.size()); ++i) {
        Value value = value_from_sqlite_column(stmt, i);
        auto& column = chunk->columns[static_cast<size_t>(i)];
        if (column.type == Value::Type::Null && !value.is_null()) {
            column.type = value.type();
        }
        column.values.push_back(std::move(value));
    }
    ++chunk->row_count;
}

std::shared_ptr<ObjectValue> row_with_group(const std::shared_ptr<ObjectValue>& row,
                                            const std::vector<std::string>& group_by) {
    if (row == nullptr) {
        return nullptr;
    }
    std::vector<std::pair<std::string, Value>> group_props;
    group_props.reserve(group_by.size());
    for (const auto& column : group_by) {
        const Value* value = row->lookup(column);
        if (value != nullptr) {
            group_props.emplace_back(column, *value);
        }
    }
    std::vector<std::pair<std::string, Value>> props = row->properties;
    props.emplace_back("_group", Value::object(std::move(group_props)));
    return std::make_shared<ObjectValue>(std::move(props));
}

} // namespace

SQLiteSource::SQLiteSource(std::string dsn, std::string table)
    : dsn_(std::move(dsn)), table_(std::move(table)), query_(query_for_table(table_)) {}

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
            auto column_stats_or = prepare_statement(
                db_or->get(), absl::StrCat("SELECT COUNT(DISTINCT ", quote_identifier(column.name),
                                           "), SUM(CASE WHEN ", quote_identifier(column.name),
                                           " IS NULL THEN 1 ELSE 0 END), AVG(LENGTH(CAST(",
                                           quote_identifier(column.name), " AS TEXT))) FROM (",
                                           query_, ") AS flux_source"));
            if (!column_stats_or.ok()) {
                statistics.columns.push_back({.name = column.name});
                continue;
            }
            std::optional<double> distinct_values;
            std::optional<double> null_fraction;
            std::optional<double> average_width_bytes;
            const int column_step_rc = sqlite3_step(column_stats_or->get());
            if (column_step_rc == SQLITE_ROW) {
                if (sqlite3_column_type(column_stats_or->get(), 0) != SQLITE_NULL) {
                    distinct_values =
                        static_cast<double>(sqlite3_column_int64(column_stats_or->get(), 0));
                }
                if (statistics.row_count.has_value() && *statistics.row_count > 0.0 &&
                    sqlite3_column_type(column_stats_or->get(), 1) != SQLITE_NULL) {
                    null_fraction =
                        static_cast<double>(sqlite3_column_int64(column_stats_or->get(), 1)) /
                        *statistics.row_count;
                } else if (statistics.row_count.has_value() && *statistics.row_count == 0.0) {
                    null_fraction = 0.0;
                }
                if (sqlite3_column_type(column_stats_or->get(), 2) != SQLITE_NULL) {
                    average_width_bytes = sqlite3_column_double(column_stats_or->get(), 2);
                } else {
                    average_width_bytes = 0.0;
                }
            }
            statistics.columns.push_back({.name = column.name,
                                          .distinct_values = distinct_values,
                                          .null_fraction = null_fraction,
                                          .average_width_bytes = average_width_bytes});
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
    auto bind_status = bind_scan_sql(stmt, *sql_or);
    if (!bind_status.ok()) {
        return bind_status;
    }

    std::vector<std::string> column_names = sqlite_column_names(stmt);

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

        rows.push_back(row_from_sqlite_stmt(stmt, column_names));
    }

    if (request.aggregate.has_value()) {
        std::vector<TableChunk> chunks;
        chunks.reserve(rows.size());
        for (auto& row : rows) {
            row = row_with_group(row, request.group_by);
            TableChunk chunk;
            chunk.rows.push_back(row);
            chunks.push_back(std::move(chunk));
        }
        return Value::table_stream("sqlite", std::move(chunks));
    }

    return Value::table("sqlite", std::move(rows));
}

SQLiteConnectorMetadata::SQLiteConnectorMetadata(SourceSpec spec) : spec_(std::move(spec)) {}

absl::StatusOr<TableHandle> SQLiteConnectorMetadata::GetTableHandle(const SourceSpec& spec) const {
    if (spec.source != "sqlite" && spec.driver != "sqlite") {
        return absl::InvalidArgumentError(
            absl::StrCat("sqlite metadata cannot open source: ", spec.source));
    }
    if (spec.dsn.empty()) {
        return absl::InvalidArgumentError("sqlite metadata requires dsn");
    }
    if (spec.table.empty()) {
        return absl::InvalidArgumentError("sqlite metadata requires table");
    }
    return TableHandle{
        .source = spec.source,
        .driver = spec.driver,
        .dsn = spec.dsn,
        .table = spec.table,
    };
}

absl::StatusOr<TableSchema> SQLiteConnectorMetadata::Schema(const TableHandle& table) const {
    return SQLiteSource(table.dsn, table.table).Schema();
}

SourceCapabilities SQLiteConnectorMetadata::Capabilities(const TableHandle& table) const {
    return SQLiteSource(table.dsn, table.table).Capabilities();
}

absl::StatusOr<TableStatistics> SQLiteConnectorMetadata::Statistics(
    const TableHandle& table) const {
    return SQLiteSource(table.dsn, table.table).Statistics();
}

SQLiteSplitManager::SQLiteSplitManager(size_t target_split_count)
    : target_split_count_(target_split_count == 0 ? default_sqlite_split_count()
                                                  : target_split_count) {}

absl::StatusOr<std::vector<ConnectorSplit>> SQLiteSplitManager::GetSplits(
    const TableHandle& table, const ScanRequest& request) const {
    auto single_split = [&]() {
        return std::vector<ConnectorSplit>{
            ConnectorSplit{.table = table, .request = request, .split_id = 0, .partition = "0"}};
    };

    if (request_requires_global_sql_order(request) && !request.partitioned_topn) {
        return single_split();
    }

    auto db_or = open_readonly_db(table.dsn);
    if (!db_or.ok()) {
        return db_or.status();
    }
    auto extent_or = load_rowid_extent(db_or->get(), table.table);
    if (!extent_or.ok()) {
        return single_split();
    }
    const RowidExtent extent = *extent_or;
    if (extent.row_count == 0 || extent.max_rowid < extent.min_rowid) {
        return single_split();
    }

    const uint64_t rowid_span =
        static_cast<uint64_t>(extent.max_rowid - extent.min_rowid) + static_cast<uint64_t>(1);
    const size_t split_count =
        std::min<size_t>(target_split_count_, std::max<size_t>(1, extent.row_count));
    if (split_count <= 1) {
        return single_split();
    }

    std::vector<ConnectorSplit> splits;
    splits.reserve(split_count);
    for (size_t index = 0; index < split_count; ++index) {
        const uint64_t start_delta = rowid_span * index / split_count;
        const uint64_t end_delta = rowid_span * (index + 1) / split_count;
        const int64_t lower = extent.min_rowid + static_cast<int64_t>(start_delta);
        const int64_t upper = extent.min_rowid + static_cast<int64_t>(end_delta) - 1;
        if (upper < lower) {
            continue;
        }
        splits.push_back(ConnectorSplit{
            .table = table,
            .request = request,
            .split_id = static_cast<int64_t>(splits.size()),
            .partition = absl::StrCat(lower, "-", upper),
            .rowid_lower = lower,
            .rowid_upper = upper,
        });
    }
    if (splits.empty()) {
        return single_split();
    }
    return splits;
}

struct SQLitePageSource::Impl {
    SqliteDb db;
    SqliteStmt stmt;
    ScanRequest request;
    std::optional<int64_t> rowid_lower;
    std::optional<int64_t> rowid_upper;
    std::vector<std::string> column_names;
    bool done = false;
    bool emitted_empty = false;
    bool emitted_any_row = false;
};

SQLitePageSource::SQLitePageSource(std::string dsn,
                                   std::string table,
                                   ScanRequest request,
                                   size_t rows_per_page,
                                   std::optional<int64_t> rowid_lower,
                                   std::optional<int64_t> rowid_upper,
                                   int64_t split_id)
    : impl_(std::make_unique<Impl>()),
      dsn_(std::move(dsn)),
      table_(std::move(table)),
      rows_per_page_(std::max<size_t>(1, rows_per_page)) {
    impl_->request = std::move(request);
    impl_->rowid_lower = rowid_lower;
    impl_->rowid_upper = rowid_upper;
    stats_.split_id = split_id;
}

absl::Status SQLitePageSource::Initialize() {
    auto db_or = open_readonly_db(dsn_);
    if (!db_or.ok()) {
        return db_or.status();
    }
    impl_->db = std::move(*db_or);
    auto schema_or = SQLiteSource(dsn_, table_).Schema();
    if (!schema_or.ok()) {
        return schema_or.status();
    }
    const std::string query =
        impl_->rowid_lower.has_value() && impl_->rowid_upper.has_value()
            ? query_for_table_rowid_range(table_, *impl_->rowid_lower, *impl_->rowid_upper)
            : query_for_table(table_);
    auto sql_or = build_scan_sql(query, impl_->request, *schema_or);
    if (!sql_or.ok()) {
        return sql_or.status();
    }
    auto stmt_or = prepare_statement(impl_->db.get(), sql_or->sql);
    if (!stmt_or.ok()) {
        return stmt_or.status();
    }
    auto bind_status = bind_scan_sql(stmt_or->get(), *sql_or);
    if (!bind_status.ok()) {
        return bind_status;
    }
    impl_->stmt = std::move(*stmt_or);
    impl_->column_names = sqlite_column_names(impl_->stmt.get());
    return absl::OkStatus();
}

absl::StatusOr<std::optional<Page>> SQLitePageSource::NextPage() {
    if (impl_ == nullptr || impl_->stmt == nullptr) {
        return absl::InvalidArgumentError("sqlite page source is not initialized");
    }
    if (impl_->done) {
        if (!impl_->emitted_empty && !impl_->emitted_any_row) {
            impl_->emitted_empty = true;
            Page page;
            page.bucket = "sqlite";
            page.chunks.push_back(empty_page_chunk_for_columns(impl_->column_names));
            ++stats_.pages_produced;
            return page;
        }
        stats_.finished = true;
        return std::nullopt;
    }

    PageChunk chunk = empty_page_chunk_for_columns(impl_->column_names);
    std::vector<std::shared_ptr<ObjectValue>> aggregate_rows;
    if (impl_->request.aggregate.has_value()) {
        aggregate_rows.reserve(rows_per_page_);
    }
    while (chunk.row_count < rows_per_page_ && aggregate_rows.size() < rows_per_page_) {
        const int step_rc = sqlite3_step(impl_->stmt.get());
        if (step_rc == SQLITE_DONE) {
            impl_->done = true;
            break;
        }
        if (step_rc != SQLITE_ROW) {
            return absl::InvalidArgumentError(
                absl::StrCat("sqlite step failed: ", sqlite3_errmsg(impl_->db.get())));
        }
        if (impl_->request.aggregate.has_value()) {
            auto row = row_from_sqlite_stmt(impl_->stmt.get(), impl_->column_names);
            row = row_with_group(row, impl_->request.group_by);
            aggregate_rows.push_back(std::move(row));
        } else {
            append_sqlite_stmt_to_page_chunk(impl_->stmt.get(), &chunk);
        }
    }

    const bool empty =
        impl_->request.aggregate.has_value() ? aggregate_rows.empty() : chunk.row_count == 0;
    if (empty) {
        if (!impl_->emitted_empty && !impl_->emitted_any_row) {
            impl_->emitted_empty = true;
            Page page;
            page.bucket = "sqlite";
            page.chunks.push_back(std::move(chunk));
            ++stats_.pages_produced;
            return page;
        }
        stats_.finished = true;
        return std::nullopt;
    }

    impl_->emitted_any_row = true;
    Page page;
    if (impl_->request.aggregate.has_value()) {
        std::vector<TableChunk> chunks;
        chunks.reserve(aggregate_rows.size());
        for (auto& row : aggregate_rows) {
            TableChunk aggregate_chunk;
            aggregate_chunk.rows.push_back(std::move(row));
            chunks.push_back(std::move(aggregate_chunk));
        }
        page = PageFromTableChunks("sqlite", std::move(chunks));
    } else {
        page.bucket = "sqlite";
        page.chunks.push_back(std::move(chunk));
    }
    ++stats_.pages_produced;
    stats_.rows_produced += page.row_count();
    return page;
}

ConnectorSplitStats SQLitePageSource::Stats() const { return stats_; }

bool SQLitePageSource::Finished() const { return stats_.finished; }

SQLitePageSourceProvider::SQLitePageSourceProvider(size_t rows_per_page)
    : rows_per_page_(std::max<size_t>(1, rows_per_page)) {}

absl::StatusOr<std::unique_ptr<ConnectorPageSource>> SQLitePageSourceProvider::CreatePageSource(
    const ConnectorSplit& split) const {
    auto page_source = std::make_unique<SQLitePageSource>(
        split.table.dsn, split.table.table, split.request, rows_per_page_, split.rowid_lower,
        split.rowid_upper, split.split_id);
    auto status = page_source->Initialize();
    if (!status.ok()) {
        return status;
    }
    return page_source;
}

std::unique_ptr<ConnectorRuntime> MakeSQLiteConnectorRuntime(const SourceSpec& spec) {
    auto runtime = std::make_unique<ConnectorRuntime>();
    runtime->metadata = std::make_unique<SQLiteConnectorMetadata>(spec);
    runtime->split_manager = std::make_unique<SQLiteSplitManager>();
    runtime->page_source_provider = std::make_unique<SQLitePageSourceProvider>();
    return runtime;
}

} // namespace pl::flux::connector
