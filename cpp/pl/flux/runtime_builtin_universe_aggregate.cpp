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

#include "cpp/pl/flux/runtime_builtin_universe.h"

#include "cpp/pl/flux/runtime_builtin_aggregate_helpers.h"

namespace pl {
namespace {

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

    std::vector<TableChunk> chunks;
    chunks.reserve((*table_or)->table_count());
    for (const auto& chunk : (*table_or)->tables) {
        Value accumulator = **identity_or;
        for (const auto& row : chunk.rows) {
            if (row == nullptr) {
                continue;
            }
            auto next_or = ExpressionEvaluator::Invoke(**fn_or, {Value::object(row), accumulator});
            if (!next_or.ok()) {
                return next_or.status();
            }
            if (next_or->type() != Value::Type::Object) {
                return absl::InvalidArgumentError("reduce `fn` must return an object");
            }
            accumulator = *next_or;
        }
        TableChunk next;
        next.rows.push_back(std::make_shared<ObjectValue>(accumulator.as_object()));
        chunks.push_back(std::move(next));
    }
    return table_with_chunks_like(**table_or, std::move(chunks));
}


absl::StatusOr<Value> builtin_distinct(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "distinct");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "distinct", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto column_or = optional_string_property(**object_or, "distinct", "column", "_value");
    if (!column_or.ok()) {
        return column_or.status();
    }

    std::vector<TableChunk> chunks;
    chunks.reserve((*table_or)->table_count());
    for (const auto& chunk : (*table_or)->tables) {
        std::unordered_set<std::string> seen;
        TableChunk next;
        next.rows.reserve(chunk.rows.size());
        for (const auto& row : chunk.rows) {
            if (row == nullptr) {
                continue;
            }
            const Value* value = row->lookup(*column_or);
            const std::string key = value == nullptr ? "<missing>" : value->string();
            if (!seen.insert(key).second) {
                continue;
            }
            next.rows.push_back(row);
        }
        chunks.push_back(std::move(next));
    }
    return table_with_chunks_like(**table_or, std::move(chunks));
}

absl::StatusOr<Value> builtin_count(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "count");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "count", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    std::string column = "_value";
    if (const Value* column_value = (*object_or)->lookup("column"); column_value != nullptr) {
        if (column_value->type() != Value::Type::String) {
            return absl::InvalidArgumentError("count `column` must be a string");
        }
        column = column_value->as_string();
    }

    std::vector<TableChunk> chunks;
    chunks.reserve((*table_or)->table_count());
    for (const auto& chunk : (*table_or)->tables) {
        int64_t count = 0;
        for (const auto& row : chunk.rows) {
            if (row != nullptr && row->lookup(column) != nullptr) {
                ++count;
            }
        }
        TableChunk next;
        next.rows.push_back(materialize_group_count_row(chunk, column, count));
        chunks.push_back(std::move(next));
    }
    return table_with_chunks_like(**table_or, std::move(chunks));
}

absl::StatusOr<Value> builtin_spread(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "spread");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "spread", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto column_or = optional_string_property(**object_or, "spread", "column", "_value");
    if (!column_or.ok()) {
        return column_or.status();
    }

    std::vector<TableChunk> chunks;
    chunks.reserve((*table_or)->table_count());
    for (const auto& chunk : (*table_or)->tables) {
        auto values_or = numeric_values_for_chunk(chunk, "spread", *column_or);
        if (!values_or.ok()) {
            return values_or.status();
        }
        if (values_or->empty()) {
            continue;
        }
        const auto [min_it, max_it] = std::minmax_element(values_or->begin(), values_or->end());
        TableChunk next;
        next.rows.push_back(
            materialize_group_value_row(chunk, *column_or, Value::floating(*max_it - *min_it)));
        chunks.push_back(std::move(next));
    }
    return table_with_chunks_like(**table_or, std::move(chunks));
}

absl::StatusOr<Value> builtin_quantile(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "quantile");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "quantile", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto column_or = optional_string_property(**object_or, "quantile", "column", "_value");
    if (!column_or.ok()) {
        return column_or.status();
    }
    auto quantiles_or = quantile_values_property(**object_or, "quantile", "q");
    if (!quantiles_or.ok()) {
        return quantiles_or.status();
    }

    std::vector<TableChunk> chunks;
    chunks.reserve((*table_or)->table_count());
    for (const auto& chunk : (*table_or)->tables) {
        auto values_or = numeric_values_for_chunk(chunk, "quantile", *column_or);
        if (!values_or.ok()) {
            return values_or.status();
        }
        if (values_or->empty()) {
            continue;
        }
        std::sort(values_or->begin(), values_or->end());
        TableChunk next;
        next.rows.reserve(quantiles_or->size());
        for (double q : *quantiles_or) {
            auto quantile_or = quantile_for_sorted_values(*values_or, q);
            if (!quantile_or.ok()) {
                return quantile_or.status();
            }
            auto row =
                materialize_group_value_row(chunk, *column_or, Value::floating(*quantile_or));
            Value updated = object_with_upserted_property(*row, "quantile", Value::floating(q));
            next.rows.push_back(std::make_shared<ObjectValue>(updated.as_object()));
        }
        chunks.push_back(std::move(next));
    }
    return table_with_chunks_like(**table_or, std::move(chunks));
}

absl::StatusOr<Value> builtin_median(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "median");
    if (!object_or.ok()) {
        return object_or.status();
    }
    std::vector<std::pair<std::string, Value>> properties = (*object_or)->properties;
    const Value* q = (*object_or)->lookup("q");
    if (q == nullptr) {
        properties.emplace_back("q", Value::floating(0.5));
    }
    return builtin_quantile({Value::object(std::move(properties))});
}

absl::StatusOr<Value> table_order_builtin(const std::vector<Value>& args,
                                          const std::string& name,
                                          bool descending) {
    auto object_or = require_object_argument(args, name);
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, name, "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto columns_or = optional_string_array_property(**object_or, name, "columns", {"_value"});
    if (!columns_or.ok()) {
        return columns_or.status();
    }
    auto n_or = integer_property(**object_or, name, "n");
    if (!n_or.ok()) {
        return n_or.status();
    }
    if (*n_or < 0) {
        return absl::InvalidArgumentError(absl::StrCat(name, " `n` must be non-negative"));
    }

    auto chunks = clone_table_chunks(**table_or);
    for (auto& chunk : chunks) {
        std::stable_sort(
            chunk.rows.begin(), chunk.rows.end(), [&](const auto& lhs, const auto& rhs) {
                if (lhs == nullptr || rhs == nullptr) {
                    return lhs != nullptr;
                }
                for (const auto& column : *columns_or) {
                    const int cmp = compare_values(lhs->lookup(column), rhs->lookup(column));
                    if (cmp != 0) {
                        return descending ? cmp > 0 : cmp < 0;
                    }
                }
                return false;
            });
        if (chunk.rows.size() > static_cast<size_t>(*n_or)) {
            chunk.rows.resize(static_cast<size_t>(*n_or));
        }
    }
    return table_with_chunks_like(**table_or, std::move(chunks));
}

absl::StatusOr<Value> builtin_top(const std::vector<Value>& args) {
    return table_order_builtin(args, "top", true);
}

absl::StatusOr<Value> builtin_bottom(const std::vector<Value>& args) {
    return table_order_builtin(args, "bottom", false);
}

absl::StatusOr<Value> table_single_row_builtin(const std::vector<Value>& args,
                                               const std::string& name,
                                               bool use_last) {
    auto object_or = require_object_argument(args, name);
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, name, "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    auto column_or = optional_string_property(**object_or, name, "column", "_value");
    if (!column_or.ok()) {
        return column_or.status();
    }

    std::vector<TableChunk> chunks;
    chunks.reserve((*table_or)->table_count());
    for (const auto& chunk : (*table_or)->tables) {
        TableChunk next;
        if (use_last) {
            for (auto it = chunk.rows.rbegin(); it != chunk.rows.rend(); ++it) {
                if (*it == nullptr) {
                    continue;
                }
                const Value* value = (*it)->lookup(*column_or);
                if (value != nullptr && !value->is_null()) {
                    next.rows.push_back(*it);
                    break;
                }
            }
        } else {
            for (const auto& row : chunk.rows) {
                if (row == nullptr) {
                    continue;
                }
                const Value* value = row->lookup(*column_or);
                if (value != nullptr && !value->is_null()) {
                    next.rows.push_back(row);
                    break;
                }
            }
        }
        if (!next.rows.empty()) {
            chunks.push_back(std::move(next));
        }
    }
    return table_with_chunks_like(**table_or, std::move(chunks));
}

absl::StatusOr<Value> builtin_first(const std::vector<Value>& args) {
    return table_single_row_builtin(args, "first", false);
}

absl::StatusOr<Value> builtin_last(const std::vector<Value>& args) {
    return table_single_row_builtin(args, "last", true);
}


} // namespace


void InstallUniverseAggregateBuiltins(Environment& env) {
    install_builtin(env, "sum", builtin_sum, "values");
    install_builtin(env, "mean", builtin_mean, "values");
    install_builtin(env, "min", builtin_min, "values");
    install_builtin(env, "max", builtin_max, "values");
    install_builtin(env, "reduce", builtin_reduce, "tables");
    install_builtin(env, "distinct", builtin_distinct, "tables");
    install_builtin(env, "count", builtin_count, "tables");
    install_builtin(env, "spread", builtin_spread, "tables");
    install_builtin(env, "quantile", builtin_quantile, "tables");
    install_builtin(env, "median", builtin_median, "tables");
    install_builtin(env, "first", builtin_first, "tables");
    install_builtin(env, "last", builtin_last, "tables");
    install_builtin(env, "top", builtin_top, "tables");
    install_builtin(env, "bottom", builtin_bottom, "tables");
}

bool InstallKnownUniverseAggregateBuiltin(Environment& env, const std::string& name) {
    if (name == "sum") { install_builtin(env, "sum", builtin_sum, "values"); return true; }
    if (name == "mean") { install_builtin(env, "mean", builtin_mean, "values"); return true; }
    if (name == "min") { install_builtin(env, "min", builtin_min, "values"); return true; }
    if (name == "max") { install_builtin(env, "max", builtin_max, "values"); return true; }
    if (name == "reduce") { install_builtin(env, "reduce", builtin_reduce, "tables"); return true; }
    if (name == "distinct") { install_builtin(env, "distinct", builtin_distinct, "tables"); return true; }
    if (name == "count") { install_builtin(env, "count", builtin_count, "tables"); return true; }
    if (name == "spread") { install_builtin(env, "spread", builtin_spread, "tables"); return true; }
    if (name == "quantile") { install_builtin(env, "quantile", builtin_quantile, "tables"); return true; }
    if (name == "median") { install_builtin(env, "median", builtin_median, "tables"); return true; }
    if (name == "first") { install_builtin(env, "first", builtin_first, "tables"); return true; }
    if (name == "last") { install_builtin(env, "last", builtin_last, "tables"); return true; }
    if (name == "top") { install_builtin(env, "top", builtin_top, "tables"); return true; }
    if (name == "bottom") { install_builtin(env, "bottom", builtin_bottom, "tables"); return true; }
    return false;
}

} // namespace pl
