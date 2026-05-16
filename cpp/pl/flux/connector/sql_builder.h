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

#pragma once

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "cpp/pl/flux/connector/table_source.h"
#include "cpp/pl/flux/runtime/runtime_value.h"
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace pl::flux::connector {

/// SqlDialect abstracts the syntactic differences between SQL backends
/// (identifier quoting, literal formatting, limit syntax, etc.).
class SqlDialect {
public:
    virtual ~SqlDialect() = default;

    /// Quote an identifier (column/table name) for this dialect.
    [[nodiscard]] virtual std::string QuoteIdentifier(const std::string& identifier) const = 0;

    /// Format a literal value for embedding directly in SQL.
    /// Returns a SQL-safe string representation.
    [[nodiscard]] virtual absl::StatusOr<std::string> FormatLiteral(
        const Value& value, bool normalize_time = false) const = 0;

    /// Return the source name for error messages (e.g. "sqlite", "mysql").
    [[nodiscard]] virtual std::string SourceName() const = 0;

    /// Format a LIMIT clause. Returns empty string if no limit.
    [[nodiscard]] virtual std::string FormatLimit(std::optional<int64_t> limit,
                                                  std::optional<int64_t> offset) const = 0;

    /// Return the unbounded limit string (dialect-specific).
    /// SQLite: "LIMIT -1", MySQL: "LIMIT 18446744073709551615"
    [[nodiscard]] virtual std::string UnboundedLimit() const = 0;
};

/// A bound SQL parameter plus whether string/time values should be normalized to
/// the backend's timestamp representation before binding.
struct SqlParam {
    Value value;
    bool normalize_time = false;
};

/// Parameterized SQL text and the values to bind to each placeholder.
struct ParameterizedSql {
    std::string sql;
    std::vector<SqlParam> params;
};

/// Common helper: convert a PredicateOp to its SQL operator string.
std::string PredicateOpSql(PredicateOp op);

/// Common helper: convert an AggregateFunction to its SQL name.
std::string AggregateFnSql(AggregateFunction fn);

/// Validate that a column name exists in the schema column set.
absl::Status ValidateColumn(const std::unordered_set<std::string>& schema_columns,
                            const std::string& column,
                            const std::string& source_name,
                            const std::string& context);

/// Validate the connector pushdown contract for a SQL scan request before SQL generation.
absl::Status ValidateScanRequestAgainstSchema(const ScanRequest& request,
                                              const TableSchema& schema,
                                              const std::string& source_name);

/// Build the SELECT clause based on the scan request.
/// Appends to `sql`. Returns error if column validation fails.
absl::Status BuildSelectClause(std::string* sql,
                               const ScanRequest& request,
                               const std::unordered_set<std::string>& schema_columns,
                               const SqlDialect& dialect);

/// Build the WHERE clause based on the scan request.
/// Appends to `sql`. Returns error if column validation or literal formatting fails.
absl::Status BuildWhereClause(std::string* sql,
                              const ScanRequest& request,
                              const std::unordered_set<std::string>& schema_columns,
                              const SqlDialect& dialect);

/// Build a parameterized WHERE clause. Literal values are appended to `params`
/// in placeholder order.
absl::Status BuildParameterizedWhereClause(std::string* sql,
                                           const ScanRequest& request,
                                           const std::unordered_set<std::string>& schema_columns,
                                           const SqlDialect& dialect,
                                           std::vector<SqlParam>* params);

/// Build the GROUP BY clause based on the scan request.
/// Appends to `sql`.
void BuildGroupByClause(std::string* sql, const ScanRequest& request, const SqlDialect& dialect);

/// Build the ORDER BY clause based on the scan request.
/// Appends to `sql`. Returns error if column validation fails.
absl::Status BuildOrderByClause(std::string* sql,
                                const ScanRequest& request,
                                const std::unordered_set<std::string>& schema_columns,
                                const SqlDialect& dialect);

/// Build a complete scan SQL from a base query and request using the given dialect.
/// This is the unified entry point that replaces the per-connector build_scan_sql functions.
absl::StatusOr<std::string> BuildScanSql(const std::string& base_query,
                                         const ScanRequest& request,
                                         const TableSchema& schema,
                                         const SqlDialect& dialect);

/// Build a complete parameterized scan SQL from a base query and request.
/// Connectors should prefer this path for physical execution when the backend
/// supports prepared statements.
absl::StatusOr<ParameterizedSql> BuildParameterizedScanSql(const std::string& base_query,
                                                           const ScanRequest& request,
                                                           const TableSchema& schema,
                                                           const SqlDialect& dialect);

} // namespace pl::flux::connector
