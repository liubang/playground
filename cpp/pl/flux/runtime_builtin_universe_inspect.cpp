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
// Created: 2026/04/25 10:40

#include "cpp/pl/flux/plan/physical_plan.h"
#include "cpp/pl/flux/plan/plan_node.h"
#include "cpp/pl/flux/execution/materializer.h"
#include "cpp/pl/flux/runtime_builtin_table_helpers.h"
#include "cpp/pl/flux/runtime_builtin_universe.h"

namespace pl::flux {
namespace {
using namespace detail;

absl::StatusOr<const TableValue*> materialized_table_ref(const TableValue& table, Value* storage) {
    if (table.materialized) {
        return &table;
    }
    Value value = Value::table_plan(table.bucket, table.plan, table.range_start, table.range_stop,
                                    table.result_name);
    auto materialized_or = execution::MaterializeValue(std::move(value));
    if (!materialized_or.ok()) {
        return materialized_or.status();
    }
    *storage = std::move(*materialized_or);
    return &storage->as_table();
}

absl::StatusOr<Value> builtin_yield(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "yield");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "yield", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto name_or = optional_string_property(
        **object_or, "yield", "name",
        (*table_or)->result_name.has_value() ? *(*table_or)->result_name : "_result");
    if (!name_or.ok()) {
        return name_or.status();
    }
    Value materialized_input;
    auto materialized_or = materialized_table_ref(**table_or, &materialized_input);
    if (!materialized_or.ok()) {
        return materialized_or.status();
    }
    const TableValue* table = *materialized_or;
    return Value::table_stream(table->bucket, table->tables, table->range_start,
                               table->range_stop, *name_or);
}

absl::StatusOr<Value> builtin_columns(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "columns");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "columns", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    Value materialized_input;
    auto materialized_or = materialized_table_ref(**table_or, &materialized_input);
    if (!materialized_or.ok()) {
        return materialized_or.status();
    }
    const TableValue* table = *materialized_or;

    std::vector<std::shared_ptr<ObjectValue>> rows;
    for (const auto& column : visible_columns_in_order(*table)) {
        rows.push_back(std::make_shared<ObjectValue>(
            std::vector<std::pair<std::string, Value>>{{"_value", Value::string(column)}}));
    }
    return Value::table(table->bucket, std::move(rows), table->range_start, table->range_stop);
}

absl::StatusOr<Value> builtin_keys(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "keys");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "keys", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    Value materialized_input;
    auto materialized_or = materialized_table_ref(**table_or, &materialized_input);
    if (!materialized_or.ok()) {
        return materialized_or.status();
    }
    const TableValue* table = *materialized_or;

    std::vector<std::shared_ptr<ObjectValue>> rows;
    for (const auto& column : group_columns_in_order(*table)) {
        rows.push_back(std::make_shared<ObjectValue>(
            std::vector<std::pair<std::string, Value>>{{"_value", Value::string(column)}}));
    }
    return Value::table(table->bucket, std::move(rows), table->range_start, table->range_stop);
}

absl::StatusOr<Value> builtin_find_column(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "findColumn");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "findColumn", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto fn_or = require_object_property(**object_or, "findColumn", "fn");
    if (!fn_or.ok()) {
        return fn_or.status();
    }
    auto column_or = string_property(**object_or, "findColumn", "column");
    if (!column_or.ok()) {
        return column_or.status();
    }
    Value materialized_input;
    auto materialized_or = materialized_table_ref(**table_or, &materialized_input);
    if (!materialized_or.ok()) {
        return materialized_or.status();
    }
    const TableValue* table = *materialized_or;

    auto rows_or = filter_rows_by_function(*table, **fn_or, "findColumn");
    if (!rows_or.ok()) {
        return rows_or.status();
    }

    std::vector<Value> values;
    values.reserve(rows_or->size());
    for (const auto& row : *rows_or) {
        const Value* value = row->lookup(*column_or);
        if (value != nullptr && !value->is_null()) {
            values.push_back(*value);
        }
    }
    return Value::array(std::move(values));
}

absl::StatusOr<Value> builtin_find_record(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "findRecord");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "findRecord", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto fn_or = require_object_property(**object_or, "findRecord", "fn");
    if (!fn_or.ok()) {
        return fn_or.status();
    }
    auto idx_or = optional_index_property(**object_or, "findRecord", "idx", 0);
    if (!idx_or.ok()) {
        return idx_or.status();
    }
    Value materialized_input;
    auto materialized_or = materialized_table_ref(**table_or, &materialized_input);
    if (!materialized_or.ok()) {
        return materialized_or.status();
    }
    const TableValue* table = *materialized_or;

    auto rows_or = filter_rows_by_function(*table, **fn_or, "findRecord");
    if (!rows_or.ok()) {
        return rows_or.status();
    }
    if (*idx_or >= rows_or->size()) {
        return Value::null();
    }
    return Value::object((*rows_or)[*idx_or]->properties);
}

absl::StatusOr<Value> builtin_explain(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "explain");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "explain", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto physical_or = optional_bool_property(**object_or, "explain", "physical", false);
    if (!physical_or.ok()) {
        return physical_or.status();
    }
    if (*physical_or) {
        return Value::string(plan::FormatPhysicalPlan((*table_or)->plan));
    }
    std::string out = plan::FormatPlan((*table_or)->plan);
    if (auto summary = source_pushdown_summary((*table_or)->plan); summary.has_value()) {
        out += *summary;
        out += "\n";
    }
    return Value::string(out);
}

} // namespace

void InstallUniverseInspectBuiltins(Environment& env) {
    install_builtin(env, "columns", builtin_columns, "tables");
    install_builtin(env, "keys", builtin_keys, "tables");
    install_builtin(env, "findColumn", builtin_find_column, "tables");
    install_builtin(env, "findRecord", builtin_find_record, "tables");
    install_builtin(env, "explain", builtin_explain, "tables");
    install_builtin(env, "yield", builtin_yield, "tables");
}

bool InstallKnownUniverseInspectBuiltin(Environment& env, const std::string& name) {
    if (name == "columns") {
        install_builtin(env, "columns", builtin_columns, "tables");
        return true;
    }
    if (name == "keys") {
        install_builtin(env, "keys", builtin_keys, "tables");
        return true;
    }
    if (name == "findColumn") {
        install_builtin(env, "findColumn", builtin_find_column, "tables");
        return true;
    }
    if (name == "findRecord") {
        install_builtin(env, "findRecord", builtin_find_record, "tables");
        return true;
    }
    if (name == "explain") {
        install_builtin(env, "explain", builtin_explain, "tables");
        return true;
    }
    if (name == "yield") {
        install_builtin(env, "yield", builtin_yield, "tables");
        return true;
    }
    return false;
}

} // namespace pl::flux
