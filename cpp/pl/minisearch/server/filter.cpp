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
// Created: 2026/08/17

#include "cpp/pl/minisearch/server/filter.h"

#include <vector>

namespace pl::minisearch::server {

namespace {

bool is_equality_op(const std::string& op) {
    return op == "=" || op == "!=";
}

bool is_range_op(const std::string& op) {
    return op == ">" || op == ">=" || op == "<" || op == "<=";
}

// 值类型与字段类型匹配：text/keyword ← string，numeric ← double。
bool value_matches_type(const proto::FieldValue& value, core::FieldType type) {
    switch (type) {
        case core::FieldType::kText:
        case core::FieldType::kKeyword:
            return value.kind_case() == proto::FieldValue::kS;
        case core::FieldType::kNumeric:
            return value.kind_case() == proto::FieldValue::kN;
        case core::FieldType::kVector:
            return false;
    }
    return false;
}

std::optional<std::string> validate_clause(const proto::FilterClause& clause,
                                           const core::Schema& schema) {
    const core::FieldDef* def = schema.Find(clause.field());
    if (def == nullptr) {
        return "filter on unknown field: " + clause.field();
    }
    if (def->type == core::FieldType::kVector) {
        return "filter on vector field is not supported: " + clause.field();
    }
    const std::string& op = clause.op();
    if (is_equality_op(op)) {
        if (clause.values_size() != 1) {
            return "op " + op + " requires exactly one value: " + clause.field();
        }
    } else if (is_range_op(op)) {
        if (def->type != core::FieldType::kNumeric) {
            return "range op requires a numeric field: " + clause.field();
        }
        if (clause.values_size() != 1) {
            return "op " + op + " requires exactly one value: " + clause.field();
        }
    } else if (op == "in") {
        if (clause.values_size() < 1) {
            return "op in requires at least one value: " + clause.field();
        }
    } else {
        return "unknown filter op: " + op;
    }
    for (const auto& value : clause.values()) {
        if (!value_matches_type(value, def->type)) {
            return "filter value type mismatch for field: " + clause.field();
        }
    }
    return std::nullopt;
}

bool match_clause(const proto::FilterClause& clause, const core::Document& doc) {
    const auto it = doc.fields.find(clause.field());
    if (it == doc.fields.end()) {
        // 缺字段：!= 视为匹配（值不等于任何给定值），其余不匹配
        return clause.op() == "!=";
    }
    const core::FieldValue& actual = it->second;
    const std::string& op = clause.op();

    const auto equals = [](const core::FieldValue& a, const proto::FieldValue& b) {
        if (const auto* s = std::get_if<std::string>(&a)) {
            return b.kind_case() == proto::FieldValue::kS && *s == b.s();
        }
        if (const auto* n = std::get_if<double>(&a)) {
            return b.kind_case() == proto::FieldValue::kN && *n == b.n();
        }
        return false;
    };

    if (op == "=") {
        return equals(actual, clause.values(0));
    }
    if (op == "!=") {
        return !equals(actual, clause.values(0));
    }
    if (op == "in") {
        for (const auto& value : clause.values()) {
            if (equals(actual, value)) {
                return true;
            }
        }
        return false;
    }
    // 范围操作（validate 已保证 numeric + 单值）
    const auto* n = std::get_if<double>(&actual);
    if (n == nullptr || clause.values(0).kind_case() != proto::FieldValue::kN) {
        return false;
    }
    const double target = clause.values(0).n();
    if (op == ">") {
        return *n > target;
    }
    if (op == ">=") {
        return *n >= target;
    }
    if (op == "<") {
        return *n < target;
    }
    if (op == "<=") {
        return *n <= target;
    }
    return false;
}

} // namespace

std::optional<std::string> ValidateFilter(const proto::Filter& filter, const core::Schema& schema) {
    for (const auto& clause : filter.and_()) {
        if (auto err = validate_clause(clause, schema); err.has_value()) {
            return err;
        }
    }
    return std::nullopt;
}

std::function<bool(const core::Document&)> BuildFilterPredicate(const proto::Filter& filter) {
    if (filter.and__size() == 0) {
        return nullptr; // 无过滤条件：调用方按“总是通过”处理
    }
    return [filter](const core::Document& doc) {
        for (const auto& clause : filter.and_()) {
            if (!match_clause(clause, doc)) {
                return false;
            }
        }
        return true;
    };
}

} // namespace pl::minisearch::server
