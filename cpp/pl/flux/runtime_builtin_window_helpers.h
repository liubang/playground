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

#include "cpp/pl/flux/runtime_builtin_aggregate_helpers.h"
#include "cpp/pl/flux/runtime_builtin_time_helpers.h"
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pl {

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif

namespace {

std::shared_ptr<ObjectValue> window_group_object(const TableChunk& chunk,
                                                 const std::string& start_column,
                                                 int64_t start_seconds,
                                                 const std::string& stop_column,
                                                 int64_t stop_seconds) {
    auto group = chunk_group_object(chunk);
    Value updated = object_with_upserted_property(
        *group, start_column, Value::time(format_rfc3339_seconds(start_seconds)));
    updated = object_with_upserted_property(updated.as_object(), stop_column,
                                            Value::time(format_rfc3339_seconds(stop_seconds)));
    return std::make_shared<ObjectValue>(updated.as_object());
}

std::shared_ptr<ObjectValue> row_with_window_bounds(const ObjectValue& row,
                                                    const std::string& start_column,
                                                    int64_t start_seconds,
                                                    const std::string& stop_column,
                                                    int64_t stop_seconds,
                                                    const std::shared_ptr<ObjectValue>& group) {
    Value updated = object_with_upserted_property(
        row, start_column, Value::time(format_rfc3339_seconds(start_seconds)));
    updated = object_with_upserted_property(updated.as_object(), stop_column,
                                            Value::time(format_rfc3339_seconds(stop_seconds)));
    updated = object_with_upserted_property(updated.as_object(), "_group", Value::object(group));
    return std::make_shared<ObjectValue>(updated.as_object());
}

std::unordered_set<std::string> aggregate_window_allowed_columns(
    const ObjectValue& row,
    const std::string& aggregate_column,
    const std::string& time_dst_column) {
    std::unordered_set<std::string> allowed = {"_group", "_start", "_stop", aggregate_column,
                                               time_dst_column};
    const Value* group = row.lookup("_group");
    if (group != nullptr && group->type() == Value::Type::Object) {
        for (const auto& [name, value] : group->as_object().properties) {
            (void)value;
            allowed.insert(name);
        }
    }
    return allowed;
}

std::shared_ptr<ObjectValue> aggregate_window_base_row(const ObjectValue& row,
                                                       const std::string& aggregate_column,
                                                       const std::string& time_dst_column) {
    std::vector<std::pair<std::string, Value>> properties;
    const auto allowed = aggregate_window_allowed_columns(row, aggregate_column, time_dst_column);
    properties.reserve(allowed.size());
    for (const auto& [name, value] : row.properties) {
        if (allowed.find(name) == allowed.end()) {
            continue;
        }
        properties.emplace_back(name, value);
    }
    return std::make_shared<ObjectValue>(std::move(properties));
}

std::optional<int64_t> aggregate_window_time_src_seconds(
    const std::shared_ptr<ObjectValue>& first_row,
    int64_t window_start,
    int64_t window_stop,
    const std::string& time_src_column) {
    if (time_src_column == "_start") {
        return window_start;
    }
    if (time_src_column == "_stop") {
        return window_stop;
    }
    if (first_row == nullptr) {
        return std::nullopt;
    }
    if (const Value* source = first_row->lookup(time_src_column); source != nullptr) {
        if (source->type() == Value::Type::Time) {
            return parse_rfc3339_seconds(source->as_time().literal);
        }
        if (source->type() == Value::Type::String) {
            return parse_rfc3339_seconds(source->as_string());
        }
    }
    return std::nullopt;
}

struct AggregateWindowBucket {
    std::optional<int64_t> start_seconds;
    std::string group_key;
    std::shared_ptr<ObjectValue> first_row;
    std::vector<Value> values;
};

absl::StatusOr<Value> invoke_window_aggregate(const FunctionValue& fn,
                                              const std::vector<Value>& values) {
    if (fn.kind == FunctionValue::Kind::Builtin && fn.name == "count") {
        return Value::integer(static_cast<int64_t>(values.size()));
    }
    if (fn.kind == FunctionValue::Kind::Builtin && fn.name == "first") {
        if (values.empty()) {
            return Value::null();
        }
        return values.front();
    }
    if (fn.kind == FunctionValue::Kind::Builtin && fn.name == "last") {
        if (values.empty()) {
            return Value::null();
        }
        return values.back();
    }
    return ExpressionEvaluator::Invoke(Value::function(std::make_shared<FunctionValue>(fn)),
                                       {Value::array(values)});
}

Value empty_window_aggregate_value(const FunctionValue& fn) {
    if (fn.kind == FunctionValue::Kind::Builtin && fn.name == "count") {
        return Value::integer(0);
    }
    return Value::null();
}

std::shared_ptr<ObjectValue> aggregate_window_output_row(
    const std::shared_ptr<ObjectValue>& base_row,
    const std::string& aggregate_column,
    Value aggregate_value,
    const std::optional<int64_t>& window_start,
    const std::optional<int64_t>& window_stop,
    const std::string& time_dst_column,
    const std::optional<int64_t>& time_src_seconds) {
    std::vector<std::pair<std::string, Value>> props = base_row->properties;
    props.reserve(base_row->properties.size() + 3);

    std::optional<size_t> aggregate_index;
    std::optional<size_t> start_index;
    std::optional<size_t> stop_index;
    std::optional<size_t> time_index;
    for (size_t i = 0; i < props.size(); ++i) {
        const std::string& key = props[i].first;
        if (key == aggregate_column) {
            aggregate_index = i;
        } else if (key == "_start") {
            start_index = i;
        } else if (key == "_stop") {
            stop_index = i;
        } else if (key == time_dst_column) {
            time_index = i;
        }
    }

    const auto upsert = [&](const std::string& key, std::optional<size_t>* index, Value value) {
        if (index->has_value()) {
            props[**index].second = std::move(value);
        } else {
            *index = props.size();
            props.emplace_back(key, std::move(value));
        }
    };

    upsert(aggregate_column, &aggregate_index, std::move(aggregate_value));
    if (window_start.has_value()) {
        upsert("_start", &start_index, Value::time(format_rfc3339_seconds(*window_start)));
    }
    if (window_stop.has_value()) {
        upsert("_stop", &stop_index, Value::time(format_rfc3339_seconds(*window_stop)));
    }
    if (time_src_seconds.has_value()) {
        upsert(time_dst_column, &time_index,
               Value::time(format_rfc3339_seconds(*time_src_seconds)));
    }
    return std::make_shared<ObjectValue>(std::move(props));
}


} // namespace

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

} // namespace pl
