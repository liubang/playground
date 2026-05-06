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
// Created: 2026/04/25 11:09

#pragma once

#include "cpp/pl/flux/compat.h"
#include "cpp/pl/flux/runtime_builtin_table_helpers.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace pl::flux {

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif

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

absl::Status checked_add_uint(uint64_t* target, uint64_t value, const std::string& name) {
    if (*target > std::numeric_limits<uint64_t>::max() - value) {
        return absl::InvalidArgumentError(absl::StrCat(name, " uint sum overflows uint64"));
    }
    *target += value;
    return absl::OkStatus();
}

absl::Status checked_add_int(int64_t* target, int64_t value, const std::string& name) {
    if ((value > 0 && *target > std::numeric_limits<int64_t>::max() - value) ||
        (value < 0 && *target < std::numeric_limits<int64_t>::min() - value)) {
        return absl::InvalidArgumentError(absl::StrCat(name, " int sum overflows int64"));
    }
    *target += value;
    return absl::OkStatus();
}

absl::StatusOr<int64_t> checked_uint_to_int(uint64_t value, const std::string& name) {
    if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return absl::InvalidArgumentError(absl::StrCat(name, " uint value overflows int64"));
    }
    return static_cast<int64_t>(value);
}

std::shared_ptr<ObjectValue> materialize_group_count_row(const TableChunk& chunk,
                                                         const std::string& column,
                                                         int64_t count) {
    std::vector<std::pair<std::string, Value>> properties;
    if (chunk.group_key != nullptr) {
        for (const auto& [name, value] : chunk.group_key->properties) {
            properties.emplace_back(name, value);
        }
        properties.emplace_back("_group",
                                Value::object(std::make_shared<ObjectValue>(*chunk.group_key)));
    }
    properties.emplace_back(column, Value::integer(count));
    return std::make_shared<ObjectValue>(std::move(properties));
}

std::shared_ptr<ObjectValue> materialize_group_value_row(const TableChunk& chunk,
                                                         const std::string& column,
                                                         Value value) {
    std::vector<std::pair<std::string, Value>> properties;
    if (chunk.group_key != nullptr) {
        for (const auto& [name, group_value] : chunk.group_key->properties) {
            properties.emplace_back(name, group_value);
        }
        properties.emplace_back("_group",
                                Value::object(std::make_shared<ObjectValue>(*chunk.group_key)));
    }
    properties.emplace_back(column, std::move(value));
    return std::make_shared<ObjectValue>(std::move(properties));
}

absl::StatusOr<std::vector<double>> quantile_values_property(const ObjectValue& object,
                                                             const std::string& name,
                                                             const std::string& property) {
    auto value_or = require_object_property(object, name, property);
    if (!value_or.ok()) {
        return value_or.status();
    }
    std::vector<double> quantiles;
    const Value& value = **value_or;
    auto append_quantile = [&](const Value& item) -> absl::Status {
        switch (item.type()) {
            case Value::Type::Float:
                quantiles.push_back(item.as_float());
                return absl::OkStatus();
            case Value::Type::Int:
                quantiles.push_back(static_cast<double>(item.as_int()));
                return absl::OkStatus();
            case Value::Type::UInt:
                quantiles.push_back(static_cast<double>(item.as_uint()));
                return absl::OkStatus();
            default:
                return absl::InvalidArgumentError(
                    absl::StrCat(name, " `", property, "` must be a number or array of numbers"));
        }
    };
    if (value.type() == Value::Type::Array) {
        quantiles.reserve(value.as_array().elements.size());
        for (const auto& item : value.as_array().elements) {
            auto status = append_quantile(item);
            if (!status.ok()) {
                return status;
            }
        }
    } else {
        auto status = append_quantile(value);
        if (!status.ok()) {
            return status;
        }
    }
    if (quantiles.empty()) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " `", property, "` must not be empty"));
    }
    return quantiles;
}

absl::StatusOr<double> quantile_for_sorted_values(const std::vector<double>& values, double q) {
    if (values.empty()) {
        return absl::InvalidArgumentError("quantile expects at least one numeric value");
    }
    if (q < 0.0 || q > 1.0) {
        return absl::InvalidArgumentError("quantile `q` must be between 0.0 and 1.0");
    }
    if (values.size() == 1) {
        return values.front();
    }
    const double position = q * static_cast<double>(values.size() - 1);
    const size_t lower_index = static_cast<size_t>(std::floor(position));
    const size_t upper_index = static_cast<size_t>(std::ceil(position));
    const double lower = values[lower_index];
    const double upper = values[upper_index];
    const double weight = position - static_cast<double>(lower_index);
    return lower + (upper - lower) * weight;
}

absl::StatusOr<std::vector<double>> numeric_values_for_chunk(const TableChunk& chunk,
                                                             const std::string& name,
                                                             const std::string& column) {
    std::vector<double> values;
    values.reserve(chunk.rows.size());
    for (const auto& row : chunk.rows) {
        if (row == nullptr) {
            continue;
        }
        const Value* value = row->lookup(column);
        if (value == nullptr || value->is_null()) {
            continue;
        }
        if (!is_numeric_value(*value)) {
            return absl::InvalidArgumentError(
                absl::StrCat(name, " `", column, "` must be numeric"));
        }
        values.push_back(numeric_value(*value));
    }
    return values;
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
                    auto int_value_or = checked_uint_to_int(item.as_uint(), name);
                    if (!int_value_or.ok()) {
                        return int_value_or.status();
                    }
                    auto status = checked_add_int(&summary.int_sum, *int_value_or, name);
                    if (!status.ok()) {
                        return status;
                    }
                } else {
                    auto status = checked_add_uint(&summary.uint_sum, item.as_uint(), name);
                    if (!status.ok()) {
                        return status;
                    }
                }
                break;
            case Value::Type::Int:
                if (summary.kind == NumericKind::Float) {
                    summary.float_sum += static_cast<double>(item.as_int());
                } else {
                    if (summary.kind == NumericKind::UInt) {
                        summary.kind = NumericKind::Int;
                        auto int_sum_or = checked_uint_to_int(summary.uint_sum, name);
                        if (!int_sum_or.ok()) {
                            return int_sum_or.status();
                        }
                        summary.int_sum = *int_sum_or;
                    }
                    auto status = checked_add_int(&summary.int_sum, item.as_int(), name);
                    if (!status.ok()) {
                        return status;
                    }
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
        default:
            PL_FLUX_UNREACHABLE();
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
    auto best_number = best->type() == Value::Type::Float ? best->as_float()
                       : best->type() == Value::Type::Int ? static_cast<double>(best->as_int())
                                                          : static_cast<double>(best->as_uint());
    for (size_t i = 1; i < elements.size(); ++i) {
        const auto& item = elements[i];
        if (item.type() != Value::Type::UInt && item.type() != Value::Type::Int &&
            item.type() != Value::Type::Float) {
            return absl::InvalidArgumentError(
                absl::StrCat(name, " expects an array of numeric values"));
        }
        auto number = item.type() == Value::Type::Float ? item.as_float()
                      : item.type() == Value::Type::Int ? static_cast<double>(item.as_int())
                                                        : static_cast<double>(item.as_uint());
        if ((choose_min && number < best_number) || (!choose_min && number > best_number)) {
            best = &item;
            best_number = number;
        }
    }
    return *best;
}

} // namespace

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

} // namespace pl::flux
