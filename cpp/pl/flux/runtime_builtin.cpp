// Copyright (c) 2023 The Authors. All rights reserved.
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

#include "cpp/pl/flux/runtime_builtin.h"

#include <algorithm>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/flux/runtime_eval.h"

namespace pl {
namespace {

enum class NumericKind {
    UInt,
    Int,
    Float,
};

struct NumericSummary {
    NumericKind kind = NumericKind::UInt;
    double float_sum = 0.0;
    int64_t int_sum = 0;
    uint64_t uint_sum = 0;
};

absl::StatusOr<const ArrayValue*> require_array_argument(const std::vector<Value>& args,
                                                         const std::string& name) {
    if (args.size() != 1 || args[0].type() != Value::Type::Array) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " expects exactly one array argument"));
    }
    return &args[0].as_array();
}

absl::StatusOr<const ObjectValue*> require_object_argument(const std::vector<Value>& args,
                                                           const std::string& name) {
    if (args.size() != 1 || args[0].type() != Value::Type::Object) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " expects exactly one object argument"));
    }
    return &args[0].as_object();
}

absl::StatusOr<const Value*> require_object_property(const ObjectValue& object,
                                                     const std::string& object_name,
                                                     const std::string& property) {
    const Value* value = object.lookup(property);
    if (value == nullptr) {
        return absl::InvalidArgumentError(
            absl::StrCat(object_name, " requires `", property, "`"));
    }
    return value;
}

absl::StatusOr<std::vector<std::shared_ptr<ObjectValue>>> require_table_rows(const Value& value,
                                                                             const std::string& name) {
    if (value.type() != Value::Type::Array) {
        return absl::InvalidArgumentError(absl::StrCat(name, " `rows` must be an array"));
    }
    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve(value.as_array().elements.size());
    for (const auto& item : value.as_array().elements) {
        if (item.type() != Value::Type::Object) {
            return absl::InvalidArgumentError(
                absl::StrCat(name, " `rows` must contain only objects"));
        }
        rows.push_back(std::make_shared<ObjectValue>(item.as_object()));
    }
    return rows;
}

absl::StatusOr<const TableValue*> require_table_property(const ObjectValue& object,
                                                         const std::string& name,
                                                         const std::string& property) {
    auto value_or = require_object_property(object, name, property);
    if (!value_or.ok()) {
        return value_or.status();
    }
    if ((*value_or)->type() != Value::Type::Table) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " `", property, "` must be a table"));
    }
    return &(*value_or)->as_table();
}

absl::StatusOr<const ArrayValue*> require_array_property(const ObjectValue& object,
                                                         const std::string& name,
                                                         const std::string& property) {
    auto value_or = require_object_property(object, name, property);
    if (!value_or.ok()) {
        return value_or.status();
    }
    if ((*value_or)->type() != Value::Type::Array) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " `", property, "` must be an array"));
    }
    return &(*value_or)->as_array();
}

std::optional<std::string> optional_literal_property(const ObjectValue& object,
                                                     const std::string& property) {
    const Value* value = object.lookup(property);
    if (value == nullptr) {
        return std::nullopt;
    }
    return value->string();
}

absl::StatusOr<int64_t> integer_property(const ObjectValue& object,
                                         const std::string& name,
                                         const std::string& property) {
    auto value_or = require_object_property(object, name, property);
    if (!value_or.ok()) {
        return value_or.status();
    }
    if ((*value_or)->type() == Value::Type::Int) {
        return (*value_or)->as_int();
    }
    if ((*value_or)->type() == Value::Type::UInt) {
        return static_cast<int64_t>((*value_or)->as_uint());
    }
    return absl::InvalidArgumentError(
        absl::StrCat(name, " `", property, "` must be an int or uint"));
}

absl::StatusOr<std::vector<std::string>> string_array_property(const ObjectValue& object,
                                                               const std::string& name,
                                                               const std::string& property) {
    auto array_or = require_array_property(object, name, property);
    if (!array_or.ok()) {
        return array_or.status();
    }
    std::vector<std::string> values;
    values.reserve((*array_or)->elements.size());
    for (const auto& item : (*array_or)->elements) {
        if (item.type() != Value::Type::String) {
            return absl::InvalidArgumentError(
                absl::StrCat(name, " `", property, "` must contain only strings"));
        }
        values.push_back(item.as_string());
    }
    return values;
}

std::optional<std::string> row_time_literal(const ObjectValue& row) {
    const Value* time = row.lookup("_time");
    if (time == nullptr) {
        return std::nullopt;
    }
    if (time->type() == Value::Type::Time || time->type() == Value::Type::String) {
        return time->type() == Value::Type::Time ? time->as_time().literal : time->as_string();
    }
    return std::nullopt;
}

bool row_matches_time_bounds(const ObjectValue& row,
                             const std::optional<std::string>& start,
                             const std::optional<std::string>& stop) {
    if (!start.has_value() && !stop.has_value()) {
        return true;
    }
    auto row_time = row_time_literal(row);
    if (!row_time.has_value()) {
        return true;
    }
    if (start.has_value() && *row_time < *start) {
        return false;
    }
    if (stop.has_value() && *row_time > *stop) {
        return false;
    }
    return true;
}

std::shared_ptr<ObjectValue> clone_row_with_selected_columns(const ObjectValue& row,
                                                             const std::vector<std::string>& columns,
                                                             bool keep_columns) {
    std::vector<std::pair<std::string, Value>> props;
    for (const auto& [key, value] : row.properties) {
        const bool listed =
            std::find(columns.begin(), columns.end(), key) != columns.end();
        if ((keep_columns && listed) || (!keep_columns && !listed)) {
            props.emplace_back(key, value);
        }
    }
    return std::make_shared<ObjectValue>(std::move(props));
}

absl::StatusOr<NumericSummary> summarize_numeric_array(const ArrayValue& array,
                                                       const std::string& name) {
    NumericSummary summary;
    for (const auto& item : array.elements) {
        switch (item.type()) {
            case Value::Type::UInt:
                if (summary.kind == NumericKind::Float) {
                    summary.float_sum += static_cast<double>(item.as_uint());
                } else if (summary.kind == NumericKind::Int) {
                    summary.int_sum += static_cast<int64_t>(item.as_uint());
                } else {
                    summary.uint_sum += item.as_uint();
                }
                break;
            case Value::Type::Int:
                if (summary.kind == NumericKind::Float) {
                    summary.float_sum += static_cast<double>(item.as_int());
                } else {
                    if (summary.kind == NumericKind::UInt) {
                        summary.kind = NumericKind::Int;
                        summary.int_sum = static_cast<int64_t>(summary.uint_sum);
                    }
                    summary.int_sum += item.as_int();
                }
                break;
            case Value::Type::Float:
                if (summary.kind == NumericKind::UInt) {
                    summary.float_sum = static_cast<double>(summary.uint_sum);
                } else if (summary.kind == NumericKind::Int) {
                    summary.float_sum = static_cast<double>(summary.int_sum);
                }
                summary.kind = NumericKind::Float;
                summary.float_sum += item.as_float();
                break;
            default:
                return absl::InvalidArgumentError(
                    absl::StrCat(name, " expects an array of numeric values"));
        }
    }
    return summary;
}

Value numeric_sum_value(const NumericSummary& summary) {
    switch (summary.kind) {
        case NumericKind::UInt:
            return Value::uinteger(summary.uint_sum);
        case NumericKind::Int:
            return Value::integer(summary.int_sum);
        case NumericKind::Float:
            return Value::floating(summary.float_sum);
    }
}

absl::StatusOr<Value> aggregate_min_max(const std::vector<Value>& args,
                                        const std::string& name,
                                        bool choose_min) {
    auto array_or = require_array_argument(args, name);
    if (!array_or.ok()) {
        return array_or.status();
    }
    const auto& elements = (*array_or)->elements;
    if (elements.empty()) {
        return absl::InvalidArgumentError(absl::StrCat(name, " expects a non-empty array"));
    }
    const Value* best = &elements[0];
    auto best_number = best->type() == Value::Type::Float    ? best->as_float()
                       : best->type() == Value::Type::Int    ? static_cast<double>(best->as_int())
                                                             : static_cast<double>(best->as_uint());
    for (size_t i = 1; i < elements.size(); ++i) {
        const auto& item = elements[i];
        if (item.type() != Value::Type::UInt && item.type() != Value::Type::Int &&
            item.type() != Value::Type::Float) {
            return absl::InvalidArgumentError(
                absl::StrCat(name, " expects an array of numeric values"));
        }
        auto number = item.type() == Value::Type::Float ? item.as_float()
                    : item.type() == Value::Type::Int   ? static_cast<double>(item.as_int())
                                                        : static_cast<double>(item.as_uint());
        if ((choose_min && number < best_number) || (!choose_min && number > best_number)) {
            best = &item;
            best_number = number;
        }
    }
    return *best;
}

absl::StatusOr<Value> builtin_len(const std::vector<Value>& args) {
    if (args.size() != 1) {
        return absl::InvalidArgumentError("len expects exactly one argument");
    }
    switch (args[0].type()) {
        case Value::Type::String:
            return Value::integer(static_cast<int64_t>(args[0].as_string().size()));
        case Value::Type::Array:
            return Value::integer(static_cast<int64_t>(args[0].as_array().elements.size()));
        case Value::Type::Object:
            return Value::integer(static_cast<int64_t>(args[0].as_object().properties.size()));
        default:
            return absl::InvalidArgumentError("len expects string, array, or object");
    }
}

absl::StatusOr<Value> builtin_string(const std::vector<Value>& args) {
    if (args.size() != 1) {
        return absl::InvalidArgumentError("string expects exactly one argument");
    }
    return Value::string(args[0].type() == Value::Type::String ? args[0].as_string()
                                                               : args[0].string());
}

absl::StatusOr<Value> builtin_contains(const std::vector<Value>& args) {
    if (args.size() != 1 || args[0].type() != Value::Type::Object) {
        return absl::InvalidArgumentError(
            "contains expects a single object argument with `set` and `value`");
    }
    const auto& object = args[0].as_object();
    const Value* set = object.lookup("set");
    const Value* value = object.lookup("value");
    if (set == nullptr || value == nullptr) {
        return absl::InvalidArgumentError("contains requires `set` and `value`");
    }
    if (set->type() != Value::Type::Array) {
        return absl::InvalidArgumentError("contains `set` must be an array");
    }
    for (const auto& item : set->as_array().elements) {
        if (item == *value) {
            return Value::boolean(true);
        }
    }
    return Value::boolean(false);
}

absl::StatusOr<Value> builtin_sum(const std::vector<Value>& args) {
    auto array_or = require_array_argument(args, "sum");
    if (!array_or.ok()) {
        return array_or.status();
    }
    auto summary_or = summarize_numeric_array(**array_or, "sum");
    if (!summary_or.ok()) {
        return summary_or.status();
    }
    return numeric_sum_value(*summary_or);
}

absl::StatusOr<Value> builtin_mean(const std::vector<Value>& args) {
    auto array_or = require_array_argument(args, "mean");
    if (!array_or.ok()) {
        return array_or.status();
    }
    if ((*array_or)->elements.empty()) {
        return absl::InvalidArgumentError("mean expects a non-empty array");
    }
    auto summary_or = summarize_numeric_array(**array_or, "mean");
    if (!summary_or.ok()) {
        return summary_or.status();
    }
    const auto count = static_cast<double>((*array_or)->elements.size());
    switch (summary_or->kind) {
        case NumericKind::UInt:
            return Value::floating(static_cast<double>(summary_or->uint_sum) / count);
        case NumericKind::Int:
            return Value::floating(static_cast<double>(summary_or->int_sum) / count);
        case NumericKind::Float:
            return Value::floating(summary_or->float_sum / count);
    }
}

absl::StatusOr<Value> builtin_min(const std::vector<Value>& args) {
    return aggregate_min_max(args, "min", true);
}

absl::StatusOr<Value> builtin_max(const std::vector<Value>& args) {
    return aggregate_min_max(args, "max", false);
}

absl::StatusOr<Value> builtin_from(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "from");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto bucket_or = require_object_property(**object_or, "from", "bucket");
    if (!bucket_or.ok()) {
        return bucket_or.status();
    }
    if ((*bucket_or)->type() != Value::Type::String) {
        return absl::InvalidArgumentError("from `bucket` must be a string");
    }
    std::vector<std::shared_ptr<ObjectValue>> rows;
    if (const Value* rows_value = (*object_or)->lookup("rows"); rows_value != nullptr) {
        auto rows_or = require_table_rows(*rows_value, "from");
        if (!rows_or.ok()) {
            return rows_or.status();
        }
        rows = std::move(*rows_or);
    }
    return Value::table((*bucket_or)->as_string(), std::move(rows));
}

absl::StatusOr<Value> builtin_range(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "range");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "range", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto start_or = require_object_property(**object_or, "range", "start");
    if (!start_or.ok()) {
        return start_or.status();
    }
    const auto start = (*start_or)->string();
    const auto stop = optional_literal_property(**object_or, "stop");

    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve((*table_or)->rows.size());
    for (const auto& row : (*table_or)->rows) {
        if (row != nullptr && row_matches_time_bounds(*row, start, stop)) {
            rows.push_back(std::make_shared<ObjectValue>(*row));
        }
    }
    return Value::table((*table_or)->bucket, std::move(rows), start, stop);
}

absl::StatusOr<Value> builtin_filter(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "filter");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "filter", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto fn_or = require_object_property(**object_or, "filter", "fn");
    if (!fn_or.ok()) {
        return fn_or.status();
    }
    if ((*fn_or)->type() != Value::Type::Function) {
        return absl::InvalidArgumentError("filter `fn` must be a function");
    }

    std::vector<std::shared_ptr<ObjectValue>> rows;
    for (const auto& row : (*table_or)->rows) {
        if (row == nullptr) {
            continue;
        }
        auto keep_or = ExpressionEvaluator::Invoke(**fn_or, {Value::object(row->properties)});
        if (!keep_or.ok()) {
            return keep_or.status();
        }
        if (keep_or->type() != Value::Type::Bool) {
            return absl::InvalidArgumentError("filter `fn` must return a boolean");
        }
        if (keep_or->as_bool()) {
            rows.push_back(std::make_shared<ObjectValue>(*row));
        }
    }
    return Value::table((*table_or)->bucket, std::move(rows),
                        (*table_or)->range_start, (*table_or)->range_stop);
}

absl::StatusOr<Value> builtin_map(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "map");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "map", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto fn_or = require_object_property(**object_or, "map", "fn");
    if (!fn_or.ok()) {
        return fn_or.status();
    }
    if ((*fn_or)->type() != Value::Type::Function) {
        return absl::InvalidArgumentError("map `fn` must be a function");
    }

    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve((*table_or)->rows.size());
    for (const auto& row : (*table_or)->rows) {
        if (row == nullptr) {
            continue;
        }
        auto mapped_or = ExpressionEvaluator::Invoke(**fn_or, {Value::object(row->properties)});
        if (!mapped_or.ok()) {
            return mapped_or.status();
        }
        if (mapped_or->type() != Value::Type::Object) {
            return absl::InvalidArgumentError("map `fn` must return an object");
        }
        rows.push_back(std::make_shared<ObjectValue>(mapped_or->as_object()));
    }
    return Value::table((*table_or)->bucket, std::move(rows),
                        (*table_or)->range_start, (*table_or)->range_stop);
}

absl::StatusOr<Value> builtin_limit(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "limit");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "limit", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto n_or = integer_property(**object_or, "limit", "n");
    if (!n_or.ok()) {
        return n_or.status();
    }
    if (*n_or < 0) {
        return absl::InvalidArgumentError("limit `n` must be non-negative");
    }
    int64_t offset = 0;
    if (const Value* offset_value = (*object_or)->lookup("offset"); offset_value != nullptr) {
        if (offset_value->type() == Value::Type::Int) {
            offset = offset_value->as_int();
        } else if (offset_value->type() == Value::Type::UInt) {
            offset = static_cast<int64_t>(offset_value->as_uint());
        } else {
            return absl::InvalidArgumentError("limit `offset` must be an int or uint");
        }
        if (offset < 0) {
            return absl::InvalidArgumentError("limit `offset` must be non-negative");
        }
    }

    std::vector<std::shared_ptr<ObjectValue>> rows;
    const size_t begin = static_cast<size_t>(offset);
    const size_t end = std::min((*table_or)->rows.size(), begin + static_cast<size_t>(*n_or));
    if (begin < (*table_or)->rows.size()) {
        rows.reserve(end - begin);
        for (size_t i = begin; i < end; ++i) {
            if ((*table_or)->rows[i] != nullptr) {
                rows.push_back(std::make_shared<ObjectValue>(*(*table_or)->rows[i]));
            }
        }
    }
    return Value::table((*table_or)->bucket, std::move(rows),
                        (*table_or)->range_start, (*table_or)->range_stop);
}

absl::StatusOr<Value> builtin_keep(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "keep");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "keep", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto columns_or = string_array_property(**object_or, "keep", "columns");
    if (!columns_or.ok()) {
        return columns_or.status();
    }
    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve((*table_or)->rows.size());
    for (const auto& row : (*table_or)->rows) {
        if (row != nullptr) {
            rows.push_back(clone_row_with_selected_columns(*row, *columns_or, true));
        }
    }
    return Value::table((*table_or)->bucket, std::move(rows),
                        (*table_or)->range_start, (*table_or)->range_stop);
}

absl::StatusOr<Value> builtin_drop(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "drop");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "drop", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto columns_or = string_array_property(**object_or, "drop", "columns");
    if (!columns_or.ok()) {
        return columns_or.status();
    }
    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve((*table_or)->rows.size());
    for (const auto& row : (*table_or)->rows) {
        if (row != nullptr) {
            rows.push_back(clone_row_with_selected_columns(*row, *columns_or, false));
        }
    }
    return Value::table((*table_or)->bucket, std::move(rows),
                        (*table_or)->range_start, (*table_or)->range_stop);
}

absl::StatusOr<Value> builtin_reduce(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "reduce");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "reduce", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto fn_or = require_object_property(**object_or, "reduce", "fn");
    if (!fn_or.ok()) {
        return fn_or.status();
    }
    auto identity_or = require_object_property(**object_or, "reduce", "identity");
    if (!identity_or.ok()) {
        return identity_or.status();
    }
    if ((*fn_or)->type() != Value::Type::Function) {
        return absl::InvalidArgumentError("reduce `fn` must be a function");
    }
    if ((*identity_or)->type() != Value::Type::Object) {
        return absl::InvalidArgumentError("reduce `identity` must be an object");
    }

    Value accumulator = **identity_or;
    for (const auto& row : (*table_or)->rows) {
        if (row == nullptr) {
            continue;
        }
        auto next_or = ExpressionEvaluator::Invoke(**fn_or,
                                                   {Value::object(row->properties), accumulator});
        if (!next_or.ok()) {
            return next_or.status();
        }
        if (next_or->type() != Value::Type::Object) {
            return absl::InvalidArgumentError("reduce `fn` must return an object");
        }
        accumulator = *next_or;
    }

    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.push_back(std::make_shared<ObjectValue>(accumulator.as_object()));
    return Value::table((*table_or)->bucket, std::move(rows),
                        (*table_or)->range_start, (*table_or)->range_stop);
}

void install_builtin(Environment& env,
                     const std::string& name,
                     FunctionValue::BuiltinCallback fn,
                     std::string pipe_param_name = {}) {
    auto callable = std::make_shared<FunctionValue>();
    callable->kind = FunctionValue::Kind::Builtin;
    callable->name = name;
    callable->pipe_param_name = std::move(pipe_param_name);
    callable->builtin = std::move(fn);
    env.define(name, Value::function(std::move(callable)));
}

bool install_known_builtin(Environment& env, const std::string& name) {
    if (name == "len") {
        install_builtin(env, "len", builtin_len);
        return true;
    }
    if (name == "string") {
        install_builtin(env, "string", builtin_string);
        return true;
    }
    if (name == "contains") {
        install_builtin(env, "contains", builtin_contains, "set");
        return true;
    }
    if (name == "sum") {
        install_builtin(env, "sum", builtin_sum, "values");
        return true;
    }
    if (name == "mean") {
        install_builtin(env, "mean", builtin_mean, "values");
        return true;
    }
    if (name == "min") {
        install_builtin(env, "min", builtin_min, "values");
        return true;
    }
    if (name == "max") {
        install_builtin(env, "max", builtin_max, "values");
        return true;
    }
    if (name == "from") {
        install_builtin(env, "from", builtin_from);
        return true;
    }
    if (name == "range") {
        install_builtin(env, "range", builtin_range, "tables");
        return true;
    }
    if (name == "filter") {
        install_builtin(env, "filter", builtin_filter, "tables");
        return true;
    }
    if (name == "map") {
        install_builtin(env, "map", builtin_map, "tables");
        return true;
    }
    if (name == "limit") {
        install_builtin(env, "limit", builtin_limit, "tables");
        return true;
    }
    if (name == "keep") {
        install_builtin(env, "keep", builtin_keep, "tables");
        return true;
    }
    if (name == "drop") {
        install_builtin(env, "drop", builtin_drop, "tables");
        return true;
    }
    if (name == "reduce") {
        install_builtin(env, "reduce", builtin_reduce, "tables");
        return true;
    }
    return false;
}

} // namespace

void BuiltinRegistry::Install(Environment& env) {
    install_known_builtin(env, "len");
    install_known_builtin(env, "string");
    install_known_builtin(env, "contains");
    install_known_builtin(env, "sum");
    install_known_builtin(env, "mean");
    install_known_builtin(env, "min");
    install_known_builtin(env, "max");
    install_known_builtin(env, "from");
    install_known_builtin(env, "range");
    install_known_builtin(env, "filter");
    install_known_builtin(env, "map");
    install_known_builtin(env, "limit");
    install_known_builtin(env, "keep");
    install_known_builtin(env, "drop");
    install_known_builtin(env, "reduce");
}

absl::Status BuiltinRegistry::Ensure(Environment& env, const std::string& name) {
    auto current = env.lookup(name);
    if (current.ok()) {
        if (current->type() != Value::Type::Function) {
            return absl::InvalidArgumentError(
                absl::StrCat("builtin name conflicts with non-function binding: ", name));
        }
        return absl::OkStatus();
    }
    if (install_known_builtin(env, name)) {
        return absl::OkStatus();
    }
    install_builtin(env, name, [name](const std::vector<Value>&) -> absl::StatusOr<Value> {
        return absl::UnimplementedError(
            absl::StrCat("builtin `", name, "` is declared but not implemented"));
    });
    return absl::OkStatus();
}

} // namespace pl
