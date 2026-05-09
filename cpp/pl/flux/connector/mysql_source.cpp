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

#include "cpp/pl/flux/connector/mysql_source.h"

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include <boost/asio/io_context.hpp>
#include <boost/mysql/any_connection.hpp>
#include <boost/mysql/column_type.hpp>
#include <boost/mysql/connect_params.hpp>
#include <boost/mysql/error_with_diagnostics.hpp>
#include <boost/mysql/field_view.hpp>
#include <boost/mysql/format_sql.hpp>
#include <boost/mysql/metadata_collection_view.hpp>
#include <boost/mysql/results.hpp>
#include <boost/mysql/ssl_mode.hpp>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pl::flux::connector {
namespace {

namespace asio = boost::asio;
namespace mysql = boost::mysql;

bool parse_port(std::string_view text, uint16_t* port) {
    if (text.empty()) {
        return false;
    }
    uint32_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = value * 10 + static_cast<uint32_t>(ch - '0');
        if (value > 65535) {
            return false;
        }
    }
    *port = static_cast<uint16_t>(value);
    return true;
}

absl::StatusOr<MySQLConnectionConfig> parse_mysql_url(std::string_view dsn) {
    constexpr std::string_view kPrefix = "mysql://";
    dsn.remove_prefix(kPrefix.size());

    const size_t at = dsn.find('@');
    const size_t slash = dsn.find('/', at == std::string_view::npos ? 0 : at + 1);
    if (at == std::string_view::npos || slash == std::string_view::npos ||
        slash + 1 >= dsn.size()) {
        return absl::InvalidArgumentError(
            "mysql dsn must be mysql://user:password@host[:port]/database");
    }

    const std::string_view userinfo = dsn.substr(0, at);
    const size_t colon = userinfo.find(':');
    if (colon == std::string_view::npos) {
        return absl::InvalidArgumentError("mysql dsn requires user and password");
    }

    std::string_view hostport = dsn.substr(at + 1, slash - at - 1);
    if (hostport.empty()) {
        return absl::InvalidArgumentError("mysql dsn requires host");
    }

    uint16_t port = 3306;
    std::string_view host = hostport;
    const size_t port_colon = hostport.rfind(':');
    if (port_colon != std::string_view::npos) {
        host = hostport.substr(0, port_colon);
        if (!parse_port(hostport.substr(port_colon + 1), &port)) {
            return absl::InvalidArgumentError("mysql dsn port must be a uint16");
        }
    }
    if (host.empty()) {
        return absl::InvalidArgumentError("mysql dsn requires host");
    }

    return MySQLConnectionConfig{
        .host = std::string(host),
        .port = port,
        .user = std::string(userinfo.substr(0, colon)),
        .password = std::string(userinfo.substr(colon + 1)),
        .database = std::string(dsn.substr(slash + 1)),
    };
}

absl::StatusOr<MySQLConnectionConfig> parse_tcp_dsn(std::string_view dsn) {
    const size_t at = dsn.find("@tcp(");
    const size_t close = at == std::string_view::npos ? std::string_view::npos : dsn.find(")/", at);
    if (at == std::string_view::npos || close == std::string_view::npos ||
        close + 2 >= dsn.size()) {
        return absl::InvalidArgumentError(
            "mysql dsn must be user:password@tcp(host[:port])/database");
    }

    const std::string_view userinfo = dsn.substr(0, at);
    const size_t colon = userinfo.find(':');
    if (colon == std::string_view::npos) {
        return absl::InvalidArgumentError("mysql dsn requires user and password");
    }

    std::string_view hostport = dsn.substr(at + 5, close - at - 5);
    if (hostport.empty()) {
        return absl::InvalidArgumentError("mysql dsn requires host");
    }

    uint16_t port = 3306;
    std::string_view host = hostport;
    const size_t port_colon = hostport.rfind(':');
    if (port_colon != std::string_view::npos) {
        host = hostport.substr(0, port_colon);
        if (!parse_port(hostport.substr(port_colon + 1), &port)) {
            return absl::InvalidArgumentError("mysql dsn port must be a uint16");
        }
    }
    if (host.empty()) {
        return absl::InvalidArgumentError("mysql dsn requires host");
    }

    return MySQLConnectionConfig{
        .host = std::string(host),
        .port = port,
        .user = std::string(userinfo.substr(0, colon)),
        .password = std::string(userinfo.substr(colon + 1)),
        .database = std::string(dsn.substr(close + 2)),
    };
}

std::string mysql_error_message(const mysql::error_with_diagnostics& err) {
    const auto server_message = err.get_diagnostics().server_message();
    if (server_message.empty()) {
        return err.what();
    }
    return absl::StrCat(err.what(), ": ", std::string(server_message));
}

absl::StatusOr<mysql::any_connection> open_connection(asio::io_context* ctx,
                                                      const std::string& dsn) {
    auto config_or = ParseMySQLDsn(dsn);
    if (!config_or.ok()) {
        return config_or.status();
    }

    mysql::connect_params params;
    params.server_address.emplace_host_and_port(config_or->host, config_or->port);
    params.username = config_or->user;
    params.password = config_or->password;
    params.database = config_or->database;
    params.ssl = mysql::ssl_mode::disable;

    try {
        mysql::any_connection conn(*ctx);
        conn.connect(params);
        return conn;
    } catch (const mysql::error_with_diagnostics& err) {
        return absl::InvalidArgumentError(
            absl::StrCat("mysql connect failed: ", mysql_error_message(err)));
    } catch (const std::exception& err) {
        return absl::InvalidArgumentError(absl::StrCat("mysql connect failed: ", err.what()));
    }
}

absl::StatusOr<mysql::results> execute_query(mysql::any_connection* conn,
                                             const std::string& sql,
                                             const std::string& context) {
    try {
        mysql::results result;
        conn->execute(sql, result);
        return result;
    } catch (const mysql::error_with_diagnostics& err) {
        return absl::InvalidArgumentError(
            absl::StrCat("mysql ", context, " failed: ", mysql_error_message(err)));
    } catch (const std::exception& err) {
        return absl::InvalidArgumentError(absl::StrCat("mysql ", context, " failed: ", err.what()));
    }
}

std::string quote_identifier(const std::string& identifier) {
    std::string out = "`";
    for (const char ch : identifier) {
        if (ch == '`') {
            out += "``";
        } else {
            out.push_back(ch);
        }
    }
    out.push_back('`');
    return out;
}

std::string predicate_op_sql(PredicateOp op) {
    switch (op) {
        case PredicateOp::Eq:
            return "=";
        case PredicateOp::NotEq:
            return "!=";
        case PredicateOp::Lt:
            return "<";
        case PredicateOp::Lte:
            return "<=";
        case PredicateOp::Gt:
            return ">";
        case PredicateOp::Gte:
            return ">=";
    }
    return "=";
}

std::string aggregate_fn_sql(AggregateFunction fn) {
    switch (fn) {
        case AggregateFunction::Count:
            return "COUNT";
        case AggregateFunction::Sum:
            return "SUM";
        case AggregateFunction::Mean:
            return "AVG";
        case AggregateFunction::Min:
            return "MIN";
        case AggregateFunction::Max:
            return "MAX";
    }
    return "COUNT";
}

bool schema_has_column(const std::unordered_set<std::string>& columns, const std::string& column) {
    return columns.find(column) != columns.end();
}

absl::Status validate_column(const std::unordered_set<std::string>& schema_columns,
                             const std::string& column,
                             const std::string& context) {
    if (!schema_has_column(schema_columns, column)) {
        return absl::InvalidArgumentError(
            absl::StrCat("mysql source ", context, " unknown column: ", column));
    }
    return absl::OkStatus();
}

absl::StatusOr<std::string> format_literal(mysql::format_options opts, const Value& value) {
    try {
        switch (value.type()) {
            case Value::Type::Null:
                return std::string("NULL");
            case Value::Type::Bool:
                return value.as_bool() ? std::string("TRUE") : std::string("FALSE");
            case Value::Type::Int:
                return mysql::format_sql(opts, "{}", value.as_int());
            case Value::Type::UInt:
                return mysql::format_sql(opts, "{}", value.as_uint());
            case Value::Type::Float:
                return mysql::format_sql(opts, "{}", value.as_float());
            case Value::Type::String:
                return mysql::format_sql(opts, "{}", value.as_string());
            case Value::Type::Time:
                return mysql::format_sql(opts, "{}", value.as_time().literal);
            default:
                return absl::InvalidArgumentError("mysql source parameter type is not pushable");
        }
    } catch (const std::exception& err) {
        return absl::InvalidArgumentError(
            absl::StrCat("mysql literal format failed: ", err.what()));
    }
}

struct BuiltSql {
    std::string sql;
};

absl::StatusOr<BuiltSql> build_scan_sql(const std::string& query,
                                        const ScanRequest& request,
                                        const TableSchema& schema,
                                        mysql::format_options opts) {
    std::unordered_set<std::string> schema_columns;
    schema_columns.reserve(schema.columns.size());
    for (const auto& column : schema.columns) {
        schema_columns.insert(column.name);
    }

    if (request.distinct.has_value()) {
        auto status = validate_column(schema_columns, *request.distinct, "distinct");
        if (!status.ok()) {
            return status;
        }
    }

    std::string sql = "SELECT ";
    if (request.aggregate.has_value()) {
        for (size_t i = 0; i < request.group_by.size(); ++i) {
            auto status = validate_column(schema_columns, request.group_by[i], "group");
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
        auto status = validate_column(schema_columns, request.aggregate->column, "aggregate");
        if (!status.ok()) {
            return status;
        }
        sql += aggregate_fn_sql(request.aggregate->fn);
        sql += "(";
        sql += quote_identifier(request.aggregate->column);
        sql += ") AS ";
        sql += quote_identifier(request.aggregate->alias.empty() ? request.aggregate->column
                                                                 : request.aggregate->alias);
    } else if (!request.projection_columns.empty()) {
        for (size_t i = 0; i < request.projection_columns.size(); ++i) {
            const auto& projection = request.projection_columns[i];
            auto status = validate_column(schema_columns, projection.column, "projection");
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
            auto status = validate_column(schema_columns, request.columns[i], "projection");
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
    if (request.time_range.has_value()) {
        auto status = validate_column(schema_columns, "_time", "time range");
        if (!status.ok()) {
            return status;
        }
        if (request.time_range->start.has_value()) {
            auto literal_or = format_literal(opts, Value::string(*request.time_range->start));
            if (!literal_or.ok()) {
                return literal_or.status();
            }
            where_clauses.push_back(quote_identifier("_time") + " >= " + *literal_or);
        }
        if (request.time_range->stop.has_value()) {
            auto literal_or = format_literal(opts, Value::string(*request.time_range->stop));
            if (!literal_or.ok()) {
                return literal_or.status();
            }
            where_clauses.push_back(quote_identifier("_time") + " < " + *literal_or);
        }
    }
    for (const auto& predicate : request.predicates) {
        auto status = validate_column(schema_columns, predicate.column, "predicate");
        if (!status.ok()) {
            return status;
        }
        auto literal_or = format_literal(opts, predicate.literal);
        if (!literal_or.ok()) {
            return literal_or.status();
        }
        where_clauses.push_back(quote_identifier(predicate.column) + " " +
                                predicate_op_sql(predicate.op) + " " + *literal_or);
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
            auto status = validate_column(schema_columns, request.order_by[i].column, "sort");
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
            return absl::InvalidArgumentError("mysql source limit must be non-negative");
        }
        sql += " LIMIT ";
        sql += std::to_string(*request.limit);
    }
    if (request.offset.has_value()) {
        if (*request.offset < 0) {
            return absl::InvalidArgumentError("mysql source offset must be non-negative");
        }
        if (!request.limit.has_value()) {
            sql += " LIMIT 18446744073709551615";
        }
        sql += " OFFSET ";
        sql += std::to_string(*request.offset);
    }

    return BuiltSql{std::move(sql)};
}

Value::Type value_type_from_metadata(const mysql::metadata& meta) {
    switch (meta.type()) {
        case mysql::column_type::tinyint:
        case mysql::column_type::smallint:
        case mysql::column_type::mediumint:
        case mysql::column_type::int_:
        case mysql::column_type::bigint:
        case mysql::column_type::year:
            return meta.is_unsigned() ? Value::Type::UInt : Value::Type::Int;
        case mysql::column_type::float_:
        case mysql::column_type::double_:
            return Value::Type::Float;
        case mysql::column_type::date:
        case mysql::column_type::datetime:
        case mysql::column_type::timestamp:
            return Value::Type::Time;
        default:
            return Value::Type::String;
    }
}

std::string format_date(mysql::date value) {
    std::ostringstream out;
    out << std::setfill('0') << std::setw(4) << value.year() << "-" << std::setw(2)
        << static_cast<int>(value.month()) << "-" << std::setw(2) << static_cast<int>(value.day());
    return out.str();
}

std::string format_datetime(mysql::datetime value) {
    std::ostringstream out;
    out << std::setfill('0') << std::setw(4) << value.year() << "-" << std::setw(2)
        << static_cast<int>(value.month()) << "-" << std::setw(2) << static_cast<int>(value.day())
        << "T" << std::setw(2) << static_cast<int>(value.hour()) << ":" << std::setw(2)
        << static_cast<int>(value.minute()) << ":" << std::setw(2)
        << static_cast<int>(value.second());
    if (value.microsecond() != 0) {
        out << "." << std::setw(6) << value.microsecond();
    }
    out << "Z";
    return out.str();
}

std::string format_time(mysql::time value) {
    const bool negative = value.count() < 0;
    auto micros = negative ? -value.count() : value.count();
    const auto hours = micros / 3600000000LL;
    micros %= 3600000000LL;
    const auto minutes = micros / 60000000LL;
    micros %= 60000000LL;
    const auto seconds = micros / 1000000LL;
    micros %= 1000000LL;
    std::ostringstream out;
    if (negative) {
        out << "-";
    }
    out << std::setfill('0') << std::setw(2) << hours << ":" << std::setw(2) << minutes << ":"
        << std::setw(2) << seconds;
    if (micros != 0) {
        out << "." << std::setw(6) << micros;
    }
    return out.str();
}

Value value_from_mysql_field(mysql::field_view field) {
    if (field.is_null()) {
        return Value::null();
    }
    if (field.is_int64()) {
        return Value::integer(field.as_int64());
    }
    if (field.is_uint64()) {
        return Value::uinteger(field.as_uint64());
    }
    if (field.is_float()) {
        return Value::floating(field.as_float());
    }
    if (field.is_double()) {
        return Value::floating(field.as_double());
    }
    if (field.is_string()) {
        const auto text = field.as_string();
        return Value::string(std::string(text.data(), text.size()));
    }
    if (field.is_blob()) {
        const auto bytes = field.as_blob();
        const auto* data = reinterpret_cast<const char*>(bytes.data());
        return Value::string(data == nullptr ? "" : std::string(data, bytes.size()));
    }
    if (field.is_date()) {
        return Value::string(format_date(field.as_date()));
    }
    if (field.is_datetime()) {
        return Value::time(format_datetime(field.as_datetime()));
    }
    if (field.is_time()) {
        return Value::string(format_time(field.as_time()));
    }
    return Value::string("");
}

absl::StatusOr<TableSchema> schema_from_result(const mysql::results& result) {
    const mysql::metadata_collection_view meta = result.meta();
    TableSchema schema;
    schema.columns.reserve(meta.size());
    for (const auto& column : meta) {
        const auto name = column.column_name();
        schema.columns.push_back({
            .name = std::string(name.data(), name.size()),
            .type = value_type_from_metadata(column),
            .nullable = !column.is_not_null(),
        });
    }
    return schema;
}

} // namespace

absl::StatusOr<MySQLConnectionConfig> ParseMySQLDsn(const std::string& dsn) {
    constexpr std::string_view kUrlPrefix = "mysql://";
    if (std::string_view(dsn).starts_with(kUrlPrefix)) {
        return parse_mysql_url(dsn);
    }
    if (dsn.find("@tcp(") != std::string::npos) {
        return parse_tcp_dsn(dsn);
    }
    return absl::InvalidArgumentError(
        "mysql dsn must be mysql://user:password@host[:port]/database or "
        "user:password@tcp(host[:port])/database");
}

MySQLSource::MySQLSource(std::string dsn, std::string table)
    : dsn_(std::move(dsn)),
      table_(std::move(table)),
      query_(absl::StrCat("SELECT * FROM ", quote_identifier(table_))) {}

absl::StatusOr<TableSchema> MySQLSource::Schema() const {
    asio::io_context ctx;
    auto conn_or = open_connection(&ctx, dsn_);
    if (!conn_or.ok()) {
        return conn_or.status();
    }
    auto result_or = execute_query(&*conn_or, query_ + " LIMIT 0", "schema query");
    if (!result_or.ok()) {
        return result_or.status();
    }
    return schema_from_result(*result_or);
}

SourceCapabilities MySQLSource::Capabilities() const {
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

absl::StatusOr<Value> MySQLSource::Scan(const ScanRequest& request) {
    asio::io_context ctx;
    auto conn_or = open_connection(&ctx, dsn_);
    if (!conn_or.ok()) {
        return conn_or.status();
    }
    auto schema_or = Schema();
    if (!schema_or.ok()) {
        return schema_or.status();
    }
    const auto opts_or = conn_or->format_opts();
    if (opts_or.has_error()) {
        return absl::InvalidArgumentError(
            absl::StrCat("mysql format options failed: ", opts_or.error().message()));
    }
    auto sql_or = build_scan_sql(query_, request, *schema_or, opts_or.value());
    if (!sql_or.ok()) {
        return sql_or.status();
    }
    auto result_or = execute_query(&*conn_or, sql_or->sql, "scan query");
    if (!result_or.ok()) {
        return result_or.status();
    }

    const mysql::metadata_collection_view meta = result_or->meta();
    std::vector<std::string> column_names;
    column_names.reserve(meta.size());
    for (const auto& column : meta) {
        const auto name = column.column_name();
        column_names.emplace_back(name.data(), name.size());
    }

    std::vector<std::shared_ptr<ObjectValue>> rows;
    for (const auto row : result_or->rows()) {
        std::vector<std::pair<std::string, Value>> properties;
        properties.reserve(column_names.size());
        for (size_t i = 0; i < column_names.size(); ++i) {
            properties.emplace_back(column_names[i], value_from_mysql_field(row.at(i)));
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
        return Value::table_stream("mysql", std::move(chunks));
    }

    return Value::table("mysql", std::move(rows));
}

} // namespace pl::flux::connector
