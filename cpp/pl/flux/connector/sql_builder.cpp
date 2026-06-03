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
// Created: 2026/05/10 16:00

#include "cpp/pl/flux/connector/sql_builder.h"

#include <algorithm>

#include "absl/strings/str_cat.h"

namespace pl::flux::connector {

std::string PredicateOpSql(PredicateOp op) {
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

std::string AggregateFnSql(AggregateFunction fn) {
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

absl::Status ValidateColumn(const std::unordered_set<std::string>& schema_columns,
                            const std::string& column,
                            const std::string& source_name,
                            const std::string& context) {
    if (schema_columns.find(column) != schema_columns.end()) {
        return absl::OkStatus();
    }
    std::vector<std::string> sorted(schema_columns.begin(), schema_columns.end());
    std::ranges::sort(sorted);
    std::string available;
    for (size_t i = 0; i < sorted.size(); ++i) {
        if (i != 0) {
            available += ", ";
        }
        available += sorted[i];
    }
    return absl::InvalidArgumentError(
        absl::StrCat(source_name,
                     " source ",
                     context,
                     " unknown column: ",
                     column,
                     available.empty() ? "" : absl::StrCat("; available columns: ", available)));
}

absl::Status ValidateLimitOffset(const ScanRequest& request, const std::string& source_name) {
    if (request.limit.has_value() && *request.limit < 0) {
        return absl::InvalidArgumentError(
            absl::StrCat(source_name, " source limit must be non-negative"));
    }
    if (request.offset.has_value() && *request.offset < 0) {
        return absl::InvalidArgumentError(
            absl::StrCat(source_name, " source offset must be non-negative"));
    }
    return absl::OkStatus();
}

absl::Status ValidateScanRequestAgainstSchema(const ScanRequest& request,
                                              const TableSchema& schema,
                                              const std::string& source_name) {
    std::unordered_set<std::string> schema_columns;
    schema_columns.reserve(schema.columns.size());
    for (const auto& column : schema.columns) {
        schema_columns.insert(column.name);
    }

    if (!request.columns.empty() && !request.projection_columns.empty()) {
        return absl::InvalidArgumentError(absl::StrCat(
            source_name, " source scan request cannot set both columns and projection_columns"));
    }
    if (request.aggregate.has_value() && request.distinct.has_value()) {
        return absl::InvalidArgumentError(absl::StrCat(
            source_name, " source scan request cannot combine aggregate and distinct"));
    }
    if (!request.group_by.empty() && !request.aggregate.has_value()) {
        return absl::InvalidArgumentError(
            absl::StrCat(source_name, " source group_by requires aggregate pushdown"));
    }

    for (const auto& column : request.columns) {
        auto status = ValidateColumn(schema_columns, column, source_name, "projection");
        if (!status.ok()) {
            return status;
        }
        if (request.distinct.has_value() && column != *request.distinct) {
            return absl::InvalidArgumentError(
                absl::StrCat(source_name,
                             " source distinct projection must use distinct column: ",
                             *request.distinct));
        }
    }
    for (const auto& projection : request.projection_columns) {
        auto status = ValidateColumn(schema_columns, projection.column, source_name, "projection");
        if (!status.ok()) {
            return status;
        }
        if (request.distinct.has_value() && projection.column != *request.distinct) {
            return absl::InvalidArgumentError(
                absl::StrCat(source_name,
                             " source distinct projection must use distinct column: ",
                             *request.distinct));
        }
    }
    if (request.time_range.has_value()) {
        auto status = ValidateColumn(schema_columns, "_time", source_name, "time range");
        if (!status.ok()) {
            return status;
        }
    }
    for (const auto& predicate : request.predicates) {
        auto status = ValidateColumn(schema_columns, predicate.column, source_name, "predicate");
        if (!status.ok()) {
            return status;
        }
    }
    for (const auto& column : request.group_by) {
        auto status = ValidateColumn(schema_columns, column, source_name, "group");
        if (!status.ok()) {
            return status;
        }
    }
    if (request.aggregate.has_value()) {
        auto status =
            ValidateColumn(schema_columns, request.aggregate->column, source_name, "aggregate");
        if (!status.ok()) {
            return status;
        }
    }
    if (request.distinct.has_value()) {
        auto status = ValidateColumn(schema_columns, *request.distinct, source_name, "distinct");
        if (!status.ok()) {
            return status;
        }
    }
    for (const auto& order_by : request.order_by) {
        auto status = ValidateColumn(schema_columns, order_by.column, source_name, "sort");
        if (!status.ok()) {
            return status;
        }
    }
    return ValidateLimitOffset(request, source_name);
}

absl::Status BuildSelectClause(std::string* sql,
                               const ScanRequest& request,
                               const std::unordered_set<std::string>& schema_columns,
                               const SqlDialect& dialect) {
    const std::string source_name = dialect.SourceName();

    if (request.distinct.has_value()) {
        auto status = ValidateColumn(schema_columns, *request.distinct, source_name, "distinct");
        if (!status.ok()) {
            return status;
        }
    }

    *sql += "SELECT ";
    if (request.aggregate.has_value()) {
        for (size_t i = 0; i < request.group_by.size(); ++i) {
            auto status = ValidateColumn(schema_columns, request.group_by[i], source_name, "group");
            if (!status.ok()) {
                return status;
            }
            if (i != 0) {
                *sql += ", ";
            }
            *sql += dialect.QuoteIdentifier(request.group_by[i]);
        }
        if (!request.group_by.empty()) {
            *sql += ", ";
        }
        auto status =
            ValidateColumn(schema_columns, request.aggregate->column, source_name, "aggregate");
        if (!status.ok()) {
            return status;
        }
        *sql += AggregateFnSql(request.aggregate->fn);
        *sql += "(";
        *sql += dialect.QuoteIdentifier(request.aggregate->column);
        *sql += ") AS ";
        *sql +=
            dialect.QuoteIdentifier(request.aggregate->alias.empty() ? request.aggregate->column
                                                                     : request.aggregate->alias);
    } else if (!request.projection_columns.empty()) {
        for (size_t i = 0; i < request.projection_columns.size(); ++i) {
            const auto& projection = request.projection_columns[i];
            auto status =
                ValidateColumn(schema_columns, projection.column, source_name, "projection");
            if (!status.ok()) {
                return status;
            }
            if (i != 0) {
                *sql += ", ";
            }
            *sql += dialect.QuoteIdentifier(projection.column);
            if (!projection.alias.empty() && projection.alias != projection.column) {
                *sql += " AS ";
                *sql += dialect.QuoteIdentifier(projection.alias);
            }
        }
    } else if (request.distinct.has_value() && request.columns.empty()) {
        *sql += dialect.QuoteIdentifier(*request.distinct);
    } else if (request.columns.empty()) {
        *sql += "*";
    } else {
        for (size_t i = 0; i < request.columns.size(); ++i) {
            auto status =
                ValidateColumn(schema_columns, request.columns[i], source_name, "projection");
            if (!status.ok()) {
                return status;
            }
            if (i != 0) {
                *sql += ", ";
            }
            *sql += dialect.QuoteIdentifier(request.columns[i]);
        }
    }
    return absl::OkStatus();
}

absl::Status BuildWhereClause(std::string* sql,
                              const ScanRequest& request,
                              const std::unordered_set<std::string>& schema_columns,
                              const SqlDialect& dialect) {
    const std::string source_name = dialect.SourceName();
    std::vector<std::string> where_clauses;

    if (request.time_range.has_value()) {
        auto status = ValidateColumn(schema_columns, "_time", source_name, "time range");
        if (!status.ok()) {
            return status;
        }
        if (request.time_range->start.has_value()) {
            auto literal_or =
                dialect.FormatLiteral(runtime::Value::string(*request.time_range->start), true);
            if (!literal_or.ok()) {
                return literal_or.status();
            }
            where_clauses.push_back(dialect.QuoteIdentifier("_time") + " >= " + *literal_or);
        }
        if (request.time_range->stop.has_value()) {
            auto literal_or =
                dialect.FormatLiteral(runtime::Value::string(*request.time_range->stop), true);
            if (!literal_or.ok()) {
                return literal_or.status();
            }
            where_clauses.push_back(dialect.QuoteIdentifier("_time") + " < " + *literal_or);
        }
    }
    for (const auto& predicate : request.predicates) {
        auto status = ValidateColumn(schema_columns, predicate.column, source_name, "predicate");
        if (!status.ok()) {
            return status;
        }
        auto literal_or = dialect.FormatLiteral(predicate.literal, predicate.column == "_time");
        if (!literal_or.ok()) {
            return literal_or.status();
        }
        where_clauses.push_back(dialect.QuoteIdentifier(predicate.column) + " " +
                                PredicateOpSql(predicate.op) + " " + *literal_or);
    }

    if (!where_clauses.empty()) {
        *sql += " WHERE ";
        for (size_t i = 0; i < where_clauses.size(); ++i) {
            if (i != 0) {
                *sql += " AND ";
            }
            *sql += where_clauses[i];
        }
    }
    return absl::OkStatus();
}

absl::Status BuildParameterizedWhereClause(std::string* sql,
                                           const ScanRequest& request,
                                           const std::unordered_set<std::string>& schema_columns,
                                           const SqlDialect& dialect,
                                           std::vector<SqlParam>* params) {
    const std::string source_name = dialect.SourceName();
    std::vector<std::string> where_clauses;

    if (request.time_range.has_value()) {
        auto status = ValidateColumn(schema_columns, "_time", source_name, "time range");
        if (!status.ok()) {
            return status;
        }
        if (request.time_range->start.has_value()) {
            where_clauses.push_back(dialect.QuoteIdentifier("_time") + " >= ?");
            params->push_back(SqlParam{
                .value = runtime::Value::string(*request.time_range->start),
                .normalize_time = true,
            });
        }
        if (request.time_range->stop.has_value()) {
            where_clauses.push_back(dialect.QuoteIdentifier("_time") + " < ?");
            params->push_back(SqlParam{
                .value = runtime::Value::string(*request.time_range->stop),
                .normalize_time = true,
            });
        }
    }
    for (const auto& predicate : request.predicates) {
        auto status = ValidateColumn(schema_columns, predicate.column, source_name, "predicate");
        if (!status.ok()) {
            return status;
        }
        where_clauses.push_back(dialect.QuoteIdentifier(predicate.column) + " " +
                                PredicateOpSql(predicate.op) + " ?");
        params->push_back(SqlParam{
            .value = predicate.literal,
            .normalize_time = predicate.column == "_time",
        });
    }

    if (!where_clauses.empty()) {
        *sql += " WHERE ";
        for (size_t i = 0; i < where_clauses.size(); ++i) {
            if (i != 0) {
                *sql += " AND ";
            }
            *sql += where_clauses[i];
        }
    }
    return absl::OkStatus();
}

void BuildGroupByClause(std::string* sql, const ScanRequest& request, const SqlDialect& dialect) {
    if (request.aggregate.has_value() && !request.group_by.empty()) {
        *sql += " GROUP BY ";
        for (size_t i = 0; i < request.group_by.size(); ++i) {
            if (i != 0) {
                *sql += ", ";
            }
            *sql += dialect.QuoteIdentifier(request.group_by[i]);
        }
    } else if (request.distinct.has_value()) {
        *sql += " GROUP BY ";
        *sql += dialect.QuoteIdentifier(*request.distinct);
    }
}

absl::Status BuildOrderByClause(std::string* sql,
                                const ScanRequest& request,
                                const std::unordered_set<std::string>& schema_columns,
                                const SqlDialect& dialect) {
    if (request.order_by.empty()) {
        return absl::OkStatus();
    }
    const std::string source_name = dialect.SourceName();
    *sql += " ORDER BY ";
    for (size_t i = 0; i < request.order_by.size(); ++i) {
        auto status =
            ValidateColumn(schema_columns, request.order_by[i].column, source_name, "sort");
        if (!status.ok()) {
            return status;
        }
        if (i != 0) {
            *sql += ", ";
        }
        *sql += dialect.QuoteIdentifier(request.order_by[i].column);
        *sql += request.order_by[i].desc ? " DESC" : " ASC";
    }
    return absl::OkStatus();
}

absl::StatusOr<std::string> BuildScanSql(const std::string& base_query,
                                         const ScanRequest& request,
                                         const TableSchema& schema,
                                         const SqlDialect& dialect) {
    auto status = ValidateScanRequestAgainstSchema(request, schema, dialect.SourceName());
    if (!status.ok()) {
        return status;
    }

    std::unordered_set<std::string> schema_columns;
    schema_columns.reserve(schema.columns.size());
    for (const auto& column : schema.columns) {
        schema_columns.insert(column.name);
    }

    std::string sql;
    status = BuildSelectClause(&sql, request, schema_columns, dialect);
    if (!status.ok()) {
        return status;
    }

    sql += " FROM (";
    sql += base_query;
    sql += ") AS flux_source";

    status = BuildWhereClause(&sql, request, schema_columns, dialect);
    if (!status.ok()) {
        return status;
    }

    BuildGroupByClause(&sql, request, dialect);

    status = BuildOrderByClause(&sql, request, schema_columns, dialect);
    if (!status.ok()) {
        return status;
    }

    sql += dialect.FormatLimit(request.limit, request.offset);

    return sql;
}

absl::StatusOr<ParameterizedSql> BuildParameterizedScanSql(const std::string& base_query,
                                                           const ScanRequest& request,
                                                           const TableSchema& schema,
                                                           const SqlDialect& dialect) {
    return BuildParameterizedScanSql(ParameterizedSql{.sql = base_query}, request, schema, dialect);
}

absl::StatusOr<ParameterizedSql> BuildParameterizedScanSql(ParameterizedSql base_query,
                                                           const ScanRequest& request,
                                                           const TableSchema& schema,
                                                           const SqlDialect& dialect) {
    auto status = ValidateScanRequestAgainstSchema(request, schema, dialect.SourceName());
    if (!status.ok()) {
        return status;
    }

    std::unordered_set<std::string> schema_columns;
    schema_columns.reserve(schema.columns.size());
    for (const auto& column : schema.columns) {
        schema_columns.insert(column.name);
    }

    ParameterizedSql out{.params = std::move(base_query.params)};
    status = BuildSelectClause(&out.sql, request, schema_columns, dialect);
    if (!status.ok()) {
        return status;
    }

    out.sql += " FROM (";
    out.sql += base_query.sql;
    out.sql += ") AS flux_source";

    status = BuildParameterizedWhereClause(&out.sql, request, schema_columns, dialect, &out.params);
    if (!status.ok()) {
        return status;
    }

    BuildGroupByClause(&out.sql, request, dialect);

    status = BuildOrderByClause(&out.sql, request, schema_columns, dialect);
    if (!status.ok()) {
        return status;
    }

    if (request.limit.has_value()) {
        out.sql += " LIMIT ?";
        out.params.push_back(SqlParam{.value = runtime::Value::integer(*request.limit)});
    }
    if (request.offset.has_value()) {
        if (!request.limit.has_value()) {
            out.sql += " ";
            out.sql += dialect.UnboundedLimit();
        }
        out.sql += " OFFSET ?";
        out.params.push_back(SqlParam{.value = runtime::Value::integer(*request.offset)});
    }

    return out;
}

} // namespace pl::flux::connector
