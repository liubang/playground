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
#include "absl/time/time.h"
#include "cpp/pl/flux/connector/sql_builder.h"
#include <algorithm>
#include <boost/asio/io_context.hpp>
#include <boost/mysql/any_connection.hpp>
#include <boost/mysql/column_type.hpp>
#include <boost/mysql/connect_params.hpp>
#include <boost/mysql/error_with_diagnostics.hpp>
#include <boost/mysql/execution_state.hpp>
#include <boost/mysql/field_view.hpp>
#include <boost/mysql/format_sql.hpp>
#include <boost/mysql/metadata_collection_view.hpp>
#include <boost/mysql/metadata_mode.hpp>
#include <boost/mysql/results.hpp>
#include <boost/mysql/rows_view.hpp>
#include <boost/mysql/ssl_mode.hpp>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
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

absl::StatusOr<bool> parse_ssl_query(std::string_view query) {
    if (query.empty()) {
        return false;
    }
    while (!query.empty()) {
        const size_t amp = query.find('&');
        const std::string_view part = amp == std::string_view::npos ? query : query.substr(0, amp);
        if (amp == std::string_view::npos) {
            query = {};
        } else {
            query.remove_prefix(amp + 1);
        }
        if (part.empty()) {
            continue;
        }
        const size_t eq = part.find('=');
        const std::string_view key = eq == std::string_view::npos ? part : part.substr(0, eq);
        const std::string_view value = eq == std::string_view::npos ? "" : part.substr(eq + 1);
        if (key != "ssl") {
            return absl::InvalidArgumentError(
                absl::StrCat("mysql dsn unsupported query parameter: ", std::string(key)));
        }
        if (value == "true" || value == "1" || value == "enable" || value == "enabled" ||
            value == "required") {
            return true;
        }
        if (value == "false" || value == "0" || value == "disable" || value == "disabled" ||
            value.empty()) {
            return false;
        }
        return absl::InvalidArgumentError("mysql dsn ssl parameter must be true or false");
    }
    return false;
}

absl::StatusOr<std::pair<std::string_view, bool>> split_database_and_query(
    std::string_view database) {
    const size_t query_start = database.find('?');
    if (query_start == std::string_view::npos) {
        return std::pair<std::string_view, bool>{database, false};
    }
    auto ssl_or = parse_ssl_query(database.substr(query_start + 1));
    if (!ssl_or.ok()) {
        return ssl_or.status();
    }
    return std::pair<std::string_view, bool>{database.substr(0, query_start), *ssl_or};
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

    auto database_or = split_database_and_query(dsn.substr(slash + 1));
    if (!database_or.ok()) {
        return database_or.status();
    }
    if (database_or->first.empty()) {
        return absl::InvalidArgumentError("mysql dsn requires database");
    }

    return MySQLConnectionConfig{
        .host = std::string(host),
        .port = port,
        .user = std::string(userinfo.substr(0, colon)),
        .password = std::string(userinfo.substr(colon + 1)),
        .database = std::string(database_or->first),
        .ssl = database_or->second,
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

    auto database_or = split_database_and_query(dsn.substr(close + 2));
    if (!database_or.ok()) {
        return database_or.status();
    }
    if (database_or->first.empty()) {
        return absl::InvalidArgumentError("mysql dsn requires database");
    }

    return MySQLConnectionConfig{
        .host = std::string(host),
        .port = port,
        .user = std::string(userinfo.substr(0, colon)),
        .password = std::string(userinfo.substr(colon + 1)),
        .database = std::string(database_or->first),
        .ssl = database_or->second,
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
    params.ssl = config_or->ssl ? mysql::ssl_mode::enable : mysql::ssl_mode::disable;

    try {
        mysql::any_connection conn(*ctx);
        conn.connect(params);
        conn.set_meta_mode(mysql::metadata_mode::full);
        mysql::results result;
        conn.execute("SET time_zone = '+00:00'", result);
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

std::string quote_table_identifier(const std::string& identifier) {
    std::string out;
    size_t start = 0;
    while (start <= identifier.size()) {
        const size_t dot = identifier.find('.', start);
        const std::string part = dot == std::string::npos ? identifier.substr(start)
                                                          : identifier.substr(start, dot - start);
        if (part.empty()) {
            return quote_identifier(identifier);
        }
        if (!out.empty()) {
            out.push_back('.');
        }
        out += quote_identifier(part);
        if (dot == std::string::npos) {
            break;
        }
        start = dot + 1;
    }
    return out;
}

// predicate_op_sql and aggregate_fn_sql are now in sql_builder.h as
// PredicateOpSql / AggregateFnSql.  validate_column is ValidateColumn.

std::optional<std::string> rfc3339_to_mysql_datetime(std::string_view literal) {
    absl::Time timestamp;
    std::string error;
    if (!absl::ParseTime(absl::RFC3339_full, literal, &timestamp, &error)) {
        return std::nullopt;
    }
    return absl::FormatTime("%Y-%m-%d %H:%M:%E6S", timestamp, absl::UTCTimeZone());
}

absl::StatusOr<std::string> format_literal(mysql::format_options opts,
                                           const Value& value,
                                           bool normalize_rfc3339_time = false) {
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
            case Value::Type::String: {
                std::string literal = value.as_string();
                if (normalize_rfc3339_time) {
                    if (auto mysql_time = rfc3339_to_mysql_datetime(literal);
                        mysql_time.has_value()) {
                        literal = *mysql_time;
                    }
                }
                return mysql::format_sql(opts, "{}", literal);
            }
            case Value::Type::Time: {
                std::string literal = value.as_time().literal;
                if (auto mysql_time = rfc3339_to_mysql_datetime(literal); mysql_time.has_value()) {
                    literal = *mysql_time;
                }
                return mysql::format_sql(opts, "{}", literal);
            }
            default:
                return absl::InvalidArgumentError("mysql source parameter type is not pushable");
        }
    } catch (const std::exception& err) {
        return absl::InvalidArgumentError(
            absl::StrCat("mysql literal format failed: ", err.what()));
    }
}

/// MySqlDialect implements SqlDialect for MySQL using Boost.MySQL format_options.
class MySqlDialect final : public SqlDialect {
public:
    explicit MySqlDialect(mysql::format_options opts) : opts_(opts) {}

    [[nodiscard]] std::string QuoteIdentifier(const std::string& identifier) const override {
        return quote_identifier(identifier);
    }

    [[nodiscard]] absl::StatusOr<std::string> FormatLiteral(const Value& value,
                                                            bool normalize_time) const override {
        return format_literal(opts_, value, normalize_time);
    }

    [[nodiscard]] std::string SourceName() const override { return "mysql"; }

    [[nodiscard]] std::string FormatLimit(std::optional<int64_t> limit,
                                          std::optional<int64_t> offset) const override {
        std::string out;
        if (limit.has_value()) {
            out += " LIMIT ";
            out += std::to_string(*limit);
        }
        if (offset.has_value()) {
            if (!limit.has_value()) {
                out += " LIMIT 18446744073709551615";
            }
            out += " OFFSET ";
            out += std::to_string(*offset);
        }
        return out;
    }

    [[nodiscard]] std::string UnboundedLimit() const override {
        return "LIMIT 18446744073709551615";
    }

private:
    mysql::format_options opts_;
};

Value::Type value_type_from_metadata(const mysql::metadata& meta) {
    switch (meta.type()) {
        case mysql::column_type::tinyint:
            if (meta.column_length() == 1) {
                return Value::Type::Bool;
            }
            return meta.is_unsigned() ? Value::Type::UInt : Value::Type::Int;
        case mysql::column_type::smallint:
        case mysql::column_type::mediumint:
        case mysql::column_type::int_:
        case mysql::column_type::bigint:
        case mysql::column_type::year:
            return meta.is_unsigned() ? Value::Type::UInt : Value::Type::Int;
        case mysql::column_type::float_:
        case mysql::column_type::double_:
            return Value::Type::Float;
        case mysql::column_type::decimal:
            return Value::Type::String;
        case mysql::column_type::date:
            return Value::Type::String;
        case mysql::column_type::datetime:
        case mysql::column_type::timestamp:
            return Value::Type::Time;
        case mysql::column_type::time:
            return Value::Type::String;
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

Value value_from_mysql_field(mysql::field_view field, const mysql::metadata& meta) {
    if (field.is_null()) {
        return Value::null();
    }
    if (meta.type() == mysql::column_type::tinyint && meta.column_length() == 1) {
        if (field.is_int64()) {
            return Value::boolean(field.as_int64() != 0);
        }
        if (field.is_uint64()) {
            return Value::boolean(field.as_uint64() != 0);
        }
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

std::optional<double> numeric_from_mysql_field(mysql::field_view field) {
    if (field.is_null()) {
        return std::nullopt;
    }
    if (field.is_int64()) {
        return static_cast<double>(field.as_int64());
    }
    if (field.is_uint64()) {
        return static_cast<double>(field.as_uint64());
    }
    if (field.is_float()) {
        return static_cast<double>(field.as_float());
    }
    if (field.is_double()) {
        return field.as_double();
    }
    return std::nullopt;
}

std::optional<int64_t> int64_from_mysql_field(mysql::field_view field) {
    if (field.is_null()) {
        return std::nullopt;
    }
    if (field.is_int64()) {
        return field.as_int64();
    }
    if (field.is_uint64()) {
        const auto value = field.as_uint64();
        if (value <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return static_cast<int64_t>(value);
        }
    }
    return std::nullopt;
}

bool request_requires_global_mysql_order(const ScanRequest& request) {
    return !request.order_by.empty() || request.limit.has_value() || request.offset.has_value() ||
           !request.group_by.empty() || request.aggregate.has_value() ||
           request.distinct.has_value();
}

bool is_integer_split_type(Value::Type type) {
    return type == Value::Type::Int || type == Value::Type::UInt;
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

std::vector<std::string> mysql_column_names(mysql::metadata_collection_view meta) {
    std::vector<std::string> column_names;
    column_names.reserve(meta.size());
    for (const auto& column : meta) {
        const auto name = column.column_name();
        column_names.emplace_back(name.data(), name.size());
    }
    return column_names;
}

std::shared_ptr<ObjectValue> row_from_mysql_view(mysql::row_view row,
                                                 mysql::metadata_collection_view meta,
                                                 const std::vector<std::string>& column_names) {
    std::vector<std::pair<std::string, Value>> properties;
    properties.reserve(column_names.size());
    for (size_t i = 0; i < column_names.size(); ++i) {
        properties.emplace_back(column_names[i], value_from_mysql_field(row.at(i), meta[i]));
    }
    return std::make_shared<ObjectValue>(std::move(properties));
}

std::shared_ptr<ObjectValue> mysql_row_with_group(const std::shared_ptr<ObjectValue>& row,
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
      query_(absl::StrCat("SELECT * FROM ", quote_table_identifier(table_))) {}

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

absl::StatusOr<TableStatistics> MySQLSource::Statistics() const {
    asio::io_context ctx;
    auto conn_or = open_connection(&ctx, dsn_);
    if (!conn_or.ok()) {
        return conn_or.status();
    }
    auto result_or = execute_query(
        &*conn_or, absl::StrCat("SELECT COUNT(*) AS row_count FROM (", query_, ") AS flux_source"),
        "statistics query");
    if (!result_or.ok()) {
        return result_or.status();
    }

    TableStatistics statistics;
    if (!result_or->rows().empty() && !result_or->rows()[0].empty()) {
        const auto field = result_or->rows()[0].at(0);
        if (field.is_int64()) {
            statistics.row_count = static_cast<double>(field.as_int64());
        } else if (field.is_uint64()) {
            statistics.row_count = static_cast<double>(field.as_uint64());
        }
    }

    auto schema_or = Schema();
    if (schema_or.ok()) {
        statistics.columns.reserve(schema_or->columns.size());
        for (const auto& column : schema_or->columns) {
            auto column_stats_or = execute_query(
                &*conn_or,
                absl::StrCat("SELECT COUNT(DISTINCT ", quote_identifier(column.name),
                             "), SUM(CASE WHEN ", quote_identifier(column.name),
                             " IS NULL THEN 1 ELSE 0 END), AVG(CHAR_LENGTH(CAST(",
                             quote_identifier(column.name), " AS CHAR))) FROM (", query_,
                             ") AS flux_source"),
                "column statistics query");
            if (!column_stats_or.ok() || column_stats_or->rows().empty() ||
                column_stats_or->rows()[0].size() < 3) {
                statistics.columns.push_back({.name = column.name});
                continue;
            }
            const auto row = column_stats_or->rows()[0];
            const auto distinct_values = numeric_from_mysql_field(row.at(0));
            std::optional<double> null_fraction;
            const auto null_count = numeric_from_mysql_field(row.at(1));
            if (statistics.row_count.has_value() && *statistics.row_count > 0.0 &&
                null_count.has_value()) {
                null_fraction = *null_count / *statistics.row_count;
            } else if (statistics.row_count.has_value() && *statistics.row_count == 0.0) {
                null_fraction = 0.0;
            }
            auto average_width_bytes = numeric_from_mysql_field(row.at(2));
            if (!average_width_bytes.has_value() && statistics.row_count.value_or(0.0) == 0.0) {
                average_width_bytes = 0.0;
            }
            statistics.columns.push_back({.name = column.name,
                                          .distinct_values = distinct_values,
                                          .null_fraction = null_fraction,
                                          .average_width_bytes = average_width_bytes});
        }
    }
    return statistics;
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
    MySqlDialect dialect(opts_or.value());
    auto sql_or = BuildScanSql(query_, request, *schema_or, dialect);
    if (!sql_or.ok()) {
        return sql_or.status();
    }
    auto result_or = execute_query(&*conn_or, *sql_or, "scan query");
    if (!result_or.ok()) {
        return result_or.status();
    }

    const mysql::metadata_collection_view meta = result_or->meta();
    std::vector<std::string> column_names = mysql_column_names(meta);

    std::vector<std::shared_ptr<ObjectValue>> rows;
    for (const auto row : result_or->rows()) {
        rows.push_back(row_from_mysql_view(row, meta, column_names));
    }

    if (request.aggregate.has_value()) {
        std::vector<TableChunk> chunks;
        chunks.reserve(rows.size());
        for (auto& row : rows) {
            row = mysql_row_with_group(row, request.group_by);
            TableChunk chunk;
            chunk.rows.push_back(row);
            chunks.push_back(std::move(chunk));
        }
        return Value::table_stream("mysql", std::move(chunks));
    }

    return Value::table("mysql", std::move(rows));
}

MySQLConnectorMetadata::MySQLConnectorMetadata(SourceSpec spec) : spec_(std::move(spec)) {}

absl::StatusOr<TableHandle> MySQLConnectorMetadata::GetTableHandle(const SourceSpec& spec) const {
    if (spec.source != "mysql" && spec.driver != "mysql") {
        return absl::InvalidArgumentError(
            absl::StrCat("mysql metadata cannot open source: ", spec.source));
    }
    if (spec.dsn.empty()) {
        return absl::InvalidArgumentError("mysql metadata requires dsn");
    }
    if (spec.table.empty()) {
        return absl::InvalidArgumentError("mysql metadata requires table");
    }
    return TableHandle{
        .source = spec.source,
        .driver = spec.driver,
        .dsn = spec.dsn,
        .table = spec.table,
    };
}

absl::StatusOr<TableSchema> MySQLConnectorMetadata::Schema(const TableHandle& table) const {
    return MySQLSource(table.dsn, table.table).Schema();
}

SourceCapabilities MySQLConnectorMetadata::Capabilities(const TableHandle& table) const {
    return MySQLSource(table.dsn, table.table).Capabilities();
}

absl::StatusOr<TableStatistics> MySQLConnectorMetadata::Statistics(const TableHandle& table) const {
    return MySQLSource(table.dsn, table.table).Statistics();
}

MySQLSplitManager::MySQLSplitManager(size_t target_split_count)
    : target_split_count_(target_split_count == 0 ? 8 : target_split_count) {}

absl::StatusOr<std::vector<ConnectorSplit>> MySQLSplitManager::GetSplits(
    const TableHandle& table, const ScanRequest& request) const {
    auto single_split = [&]() {
        return std::vector<ConnectorSplit>{
            ConnectorSplit{.table = table, .request = request, .split_id = 0, .partition = "0"}};
    };

    if (request_requires_global_mysql_order(request)) {
        return single_split();
    }

    auto schema_or = MySQLSource(table.dsn, table.table).Schema();
    if (!schema_or.ok()) {
        return schema_or.status();
    }
    std::vector<std::string> split_candidates;
    split_candidates.reserve(schema_or->columns.size());

    asio::io_context ctx;
    auto conn_or = open_connection(&ctx, table.dsn);
    if (!conn_or.ok()) {
        return conn_or.status();
    }
    const auto opts_or = conn_or->format_opts();
    if (opts_or.has_error()) {
        return absl::InvalidArgumentError(
            absl::StrCat("mysql format options failed: ", opts_or.error().message()));
    }
    auto table_literal_or = format_literal(opts_or.value(), Value::string(table.table));
    if (!table_literal_or.ok()) {
        return table_literal_or.status();
    }
    auto primary_or = execute_query(
        &*conn_or,
        absl::StrCat("SELECT COLUMN_NAME FROM INFORMATION_SCHEMA.KEY_COLUMN_USAGE "
                     "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = ",
                     *table_literal_or,
                     " AND CONSTRAINT_NAME = 'PRIMARY' ORDER BY ORDINAL_POSITION LIMIT 1"),
        "primary key discovery query");
    if (primary_or.ok() && !primary_or->rows().empty() && !primary_or->rows()[0].empty() &&
        primary_or->rows()[0].at(0).is_string()) {
        const auto text = primary_or->rows()[0].at(0).as_string();
        split_candidates.emplace_back(text.data(), text.size());
    }
    for (const auto& column : schema_or->columns) {
        if ((column.name == "id" || column.name == "seq") && is_integer_split_type(column.type)) {
            split_candidates.push_back(column.name);
        }
    }
    for (const auto& column : schema_or->columns) {
        if (is_integer_split_type(column.type)) {
            split_candidates.push_back(column.name);
        }
    }

    for (const auto& candidate : split_candidates) {
        const auto column_it = std::find_if(
            schema_or->columns.begin(), schema_or->columns.end(),
            [&](const auto& column) {
                return column.name == candidate && is_integer_split_type(column.type);
            });
        if (column_it == schema_or->columns.end()) {
            continue;
        }
        auto extent_or = execute_query(
            &*conn_or,
            absl::StrCat("SELECT MIN(", quote_identifier(candidate), "), MAX(",
                         quote_identifier(candidate), "), COUNT(*) FROM ",
                         quote_table_identifier(table.table)),
            "split extent query");
        if (!extent_or.ok() || extent_or->rows().empty() || extent_or->rows()[0].size() < 3) {
            continue;
        }
        const auto row = extent_or->rows()[0];
        const auto lower = int64_from_mysql_field(row.at(0));
        const auto upper = int64_from_mysql_field(row.at(1));
        const auto count = int64_from_mysql_field(row.at(2));
        if (!lower.has_value() || !upper.has_value() || !count.has_value() || *count <= 0 ||
            *upper < *lower) {
            continue;
        }
        const size_t row_count = static_cast<size_t>(std::max<int64_t>(1, *count));
        const size_t split_count = std::min<size_t>(target_split_count_, row_count);
        if (split_count <= 1) {
            return single_split();
        }
        const uint64_t span = static_cast<uint64_t>(*upper - *lower) + static_cast<uint64_t>(1);
        std::vector<ConnectorSplit> splits;
        splits.reserve(split_count);
        for (size_t index = 0; index < split_count; ++index) {
            const uint64_t start_delta = span * index / split_count;
            const uint64_t end_delta = span * (index + 1) / split_count;
            const int64_t split_lower = *lower + static_cast<int64_t>(start_delta);
            const int64_t split_upper = *lower + static_cast<int64_t>(end_delta) - 1;
            if (split_upper < split_lower) {
                continue;
            }
            splits.push_back(ConnectorSplit{
                .table = table,
                .request = request,
                .split_id = static_cast<int64_t>(splits.size()),
                .partition = absl::StrCat(candidate, ":", split_lower, "-", split_upper),
                .split_column = candidate,
                .split_lower = split_lower,
                .split_upper = split_upper,
            });
        }
        if (!splits.empty()) {
            return splits;
        }
    }
    return single_split();
}

MySQLPageSourceProvider::MySQLPageSourceProvider(size_t rows_per_page)
    : rows_per_page_(std::max<size_t>(1, rows_per_page)) {}

struct MySQLPageSource::Impl {
    asio::io_context ctx;
    mysql::any_connection conn;
    mysql::execution_state state;
    ScanRequest request;
    std::optional<std::string> split_column;
    std::optional<int64_t> split_lower;
    std::optional<int64_t> split_upper;
    std::vector<std::string> column_names;
    bool emitted_empty = false;
    bool emitted_any_row = false;

    Impl() : conn(ctx) {}
};

MySQLPageSource::MySQLPageSource(std::string dsn,
                                 std::string table,
                                 ScanRequest request,
                                 size_t rows_per_page,
                                 std::optional<std::string> split_column,
                                 std::optional<int64_t> split_lower,
                                 std::optional<int64_t> split_upper,
                                 int64_t split_id)
    : impl_(std::make_unique<Impl>()),
      dsn_(std::move(dsn)),
      table_(std::move(table)),
      rows_per_page_(std::max<size_t>(1, rows_per_page)) {
    impl_->request = std::move(request);
    impl_->split_column = std::move(split_column);
    impl_->split_lower = split_lower;
    impl_->split_upper = split_upper;
    stats_.split_id = split_id;
}

absl::Status MySQLPageSource::Initialize() {
    auto conn_or = open_connection(&impl_->ctx, dsn_);
    if (!conn_or.ok()) {
        return conn_or.status();
    }
    impl_->conn = std::move(*conn_or);
    auto schema_or = MySQLSource(dsn_, table_).Schema();
    if (!schema_or.ok()) {
        return schema_or.status();
    }
    const auto opts_or = impl_->conn.format_opts();
    if (opts_or.has_error()) {
        return absl::InvalidArgumentError(
            absl::StrCat("mysql format options failed: ", opts_or.error().message()));
    }
    MySqlDialect dialect(opts_or.value());
    ScanRequest effective_request = impl_->request;
    if (impl_->split_column.has_value() && impl_->split_lower.has_value() &&
        impl_->split_upper.has_value()) {
        effective_request.predicates.push_back({
            .op = PredicateOp::Gte,
            .column = *impl_->split_column,
            .literal = Value::integer(*impl_->split_lower),
        });
        effective_request.predicates.push_back({
            .op = PredicateOp::Lte,
            .column = *impl_->split_column,
            .literal = Value::integer(*impl_->split_upper),
        });
    }
    auto sql_or = BuildScanSql(absl::StrCat("SELECT * FROM ", quote_table_identifier(table_)),
                               effective_request, *schema_or, dialect);
    if (!sql_or.ok()) {
        return sql_or.status();
    }
    try {
        impl_->conn.start_execution(*sql_or, impl_->state);
        impl_->column_names = mysql_column_names(impl_->state.meta());
    } catch (const mysql::error_with_diagnostics& err) {
        return absl::InvalidArgumentError(
            absl::StrCat("mysql scan start failed: ", mysql_error_message(err)));
    } catch (const std::exception& err) {
        return absl::InvalidArgumentError(absl::StrCat("mysql scan start failed: ", err.what()));
    }
    return absl::OkStatus();
}

absl::StatusOr<std::optional<Page>> MySQLPageSource::NextPage() {
    if (impl_ == nullptr || impl_->state.should_start_op()) {
        return absl::InvalidArgumentError("mysql page source is not initialized");
    }
    if (impl_->state.complete()) {
        if (!impl_->emitted_empty && !impl_->emitted_any_row) {
            impl_->emitted_empty = true;
            Page page = PageFromRows("mysql", {});
            ++stats_.pages_produced;
            return page;
        }
        stats_.finished = true;
        return std::nullopt;
    }

    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve(rows_per_page_);
    try {
        while (rows.size() < rows_per_page_ && impl_->state.should_read_rows()) {
            mysql::rows_view batch = impl_->conn.read_some_rows(impl_->state);
            if (batch.empty()) {
                if (impl_->state.complete()) {
                    break;
                }
                return absl::InternalError("mysql scan read returned an empty batch before EOF");
            }
            const auto meta = impl_->state.meta();
            for (const auto row : batch) {
                auto object = row_from_mysql_view(row, meta, impl_->column_names);
                if (impl_->request.aggregate.has_value()) {
                    object = mysql_row_with_group(object, impl_->request.group_by);
                }
                rows.push_back(std::move(object));
            }
        }
    } catch (const mysql::error_with_diagnostics& err) {
        return absl::InvalidArgumentError(
            absl::StrCat("mysql scan read failed: ", mysql_error_message(err)));
    } catch (const std::exception& err) {
        return absl::InvalidArgumentError(absl::StrCat("mysql scan read failed: ", err.what()));
    }

    if (rows.empty()) {
        if (!impl_->emitted_empty && !impl_->emitted_any_row) {
            impl_->emitted_empty = true;
            Page page = PageFromRows("mysql", {});
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
        chunks.reserve(rows.size());
        for (auto& row : rows) {
            TableChunk chunk;
            chunk.rows.push_back(std::move(row));
            chunks.push_back(std::move(chunk));
        }
        page = PageFromTableChunks("mysql", std::move(chunks));
    } else {
        page = PageFromRows("mysql", std::move(rows));
    }
    ++stats_.pages_produced;
    stats_.rows_produced += page.row_count();
    return page;
}

ConnectorSplitStats MySQLPageSource::Stats() const { return stats_; }

bool MySQLPageSource::Finished() const { return stats_.finished; }

absl::StatusOr<std::unique_ptr<ConnectorPageSource>> MySQLPageSourceProvider::CreatePageSource(
    const ConnectorSplit& split) const {
    auto page_source = std::make_unique<MySQLPageSource>(split.table.dsn, split.table.table,
                                                         split.request, rows_per_page_,
                                                         split.split_column, split.split_lower,
                                                         split.split_upper,
                                                         split.split_id);
    auto status = page_source->Initialize();
    if (!status.ok()) {
        return status;
    }
    return page_source;
}

std::unique_ptr<ConnectorRuntime> MakeMySQLConnectorRuntime(const SourceSpec& spec) {
    auto runtime = std::make_unique<ConnectorRuntime>();
    runtime->metadata = std::make_unique<MySQLConnectorMetadata>(spec);
    runtime->split_manager = std::make_unique<MySQLSplitManager>();
    runtime->page_source_provider = std::make_unique<MySQLPageSourceProvider>();
    return runtime;
}

} // namespace pl::flux::connector
