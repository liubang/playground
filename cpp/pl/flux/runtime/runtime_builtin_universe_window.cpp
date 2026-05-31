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

#include <algorithm>

#include "cpp/pl/flux/execution/materializer.h"
#include "cpp/pl/flux/runtime/runtime_builtin_universe.h"
#include "cpp/pl/flux/runtime/runtime_builtin_window_helpers.h"

namespace pl::flux {
namespace {
using namespace detail;

absl::StatusOr<const TableValue*> materialized_table_ref(const TableValue& table, Value* storage) {
    if (table.materialized) {
        return &table;
    }
    Value value = Value::table_plan(
        table.bucket, table.plan, table.range_start, table.range_stop, table.result_name);
    auto materialized_or = execution::MaterializeValue(std::move(value));
    if (!materialized_or.ok()) {
        return materialized_or.status();
    }
    *storage = std::move(*materialized_or);
    return &storage->as_table();
}

absl::StatusOr<Value> builtin_elapsed(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "elapsed");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "elapsed", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    Value materialized_input;
    auto materialized_or = materialized_table_ref(**table_or, &materialized_input);
    if (!materialized_or.ok()) {
        return materialized_or.status();
    }
    table_or = *materialized_or;
    auto time_column_or = optional_string_property(**object_or, "elapsed", "timeColumn", "_time");
    if (!time_column_or.ok()) {
        return time_column_or.status();
    }
    auto column_name_or = optional_string_property(**object_or, "elapsed", "columnName", "elapsed");
    if (!column_name_or.ok()) {
        return column_name_or.status();
    }

    int64_t unit_seconds = 1;
    if (const Value* unit_value = (*object_or)->lookup("unit"); unit_value != nullptr) {
        auto unit_or = parse_window_duration(*unit_value, "elapsed", "unit");
        if (!unit_or.ok()) {
            return unit_or.status();
        }
        if (unit_or->kind != WindowDuration::Kind::FixedSeconds) {
            return absl::InvalidArgumentError("elapsed `unit` does not support calendar durations");
        }
        unit_seconds = unit_or->seconds;
    }

    std::unordered_map<std::string, int64_t> previous_time_by_group;
    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve((*table_or)->rows.size());
    for (const auto& row : (*table_or)->rows) {
        if (row == nullptr) {
            continue;
        }
        const Value* time_value = row->lookup(*time_column_or);
        if (time_value == nullptr) {
            return absl::InvalidArgumentError(
                absl::StrCat("elapsed requires `", *time_column_or, "` on every row"));
        }

        std::string literal;
        if (time_value->type() == Value::Type::Time) {
            literal = time_value->as_time().literal;
        } else if (time_value->type() == Value::Type::String) {
            literal = time_value->as_string();
        } else {
            return absl::InvalidArgumentError(
                absl::StrCat("elapsed `", *time_column_or, "` must be a time or string"));
        }
        auto seconds_or = parse_rfc3339_seconds(literal);
        if (!seconds_or.has_value()) {
            return absl::InvalidArgumentError(
                absl::StrCat("elapsed could not parse RFC3339 time: ", literal));
        }

        const std::string group_key = group_key_for_row(*row);
        if (const auto previous = previous_time_by_group.find(group_key);
            previous != previous_time_by_group.end()) {
            auto updated = object_with_upserted_property(
                *row,
                *column_name_or,
                Value::integer((*seconds_or - previous->second) / unit_seconds));
            rows.push_back(std::make_shared<ObjectValue>(updated.as_object()));
        }
        previous_time_by_group[group_key] = *seconds_or;
    }
    auto result = Value::table(
        (*table_or)->bucket, std::move(rows), (*table_or)->range_start, (*table_or)->range_stop);
    return with_materialization_barrier(std::move(result), **table_or, "elapsed");
}

absl::StatusOr<Value> builtin_difference(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "difference");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "difference", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    Value materialized_input;
    auto materialized_or = materialized_table_ref(**table_or, &materialized_input);
    if (!materialized_or.ok()) {
        return materialized_or.status();
    }
    table_or = *materialized_or;
    auto column_or = optional_string_property(**object_or, "difference", "column", "_value");
    if (!column_or.ok()) {
        return column_or.status();
    }
    auto non_negative_or = optional_bool_property(**object_or, "difference", "nonNegative", false);
    if (!non_negative_or.ok()) {
        return non_negative_or.status();
    }
    auto keep_first_or = optional_bool_property(**object_or, "difference", "keepFirst", false);
    if (!keep_first_or.ok()) {
        return keep_first_or.status();
    }

    std::unordered_map<std::string, Value> previous_by_group;
    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve((*table_or)->rows.size());
    for (const auto& row : (*table_or)->rows) {
        if (row == nullptr) {
            continue;
        }
        const Value* current = row->lookup(*column_or);
        if (current == nullptr) {
            return absl::InvalidArgumentError(
                absl::StrCat("difference requires `", *column_or, "` on every row"));
        }
        if (!is_numeric_value(*current)) {
            return absl::InvalidArgumentError(
                absl::StrCat("difference `", *column_or, "` must be numeric"));
        }

        const std::string group_key = group_key_for_row(*row);
        if (const auto previous = previous_by_group.find(group_key);
            previous != previous_by_group.end()) {
            const double delta = numeric_value(*current) - numeric_value(previous->second);
            Value difference = Value::null();
            if (!*non_negative_or || delta >= 0.0) {
                if (current->type() == Value::Type::Float ||
                    previous->second.type() == Value::Type::Float) {
                    difference = Value::floating(delta);
                } else {
                    difference = Value::integer(static_cast<int64_t>(delta));
                }
            }
            auto updated = object_with_upserted_property(*row, *column_or, std::move(difference));
            rows.push_back(std::make_shared<ObjectValue>(updated.as_object()));
        } else if (*keep_first_or) {
            auto updated = object_with_upserted_property(*row, *column_or, Value::null());
            rows.push_back(std::make_shared<ObjectValue>(updated.as_object()));
        }
        previous_by_group[group_key] = *current;
    }
    auto result = Value::table(
        (*table_or)->bucket, std::move(rows), (*table_or)->range_start, (*table_or)->range_stop);
    return with_materialization_barrier(std::move(result), **table_or, "difference");
}

absl::StatusOr<Value> builtin_derivative(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "derivative");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "derivative", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    Value materialized_input;
    auto materialized_or = materialized_table_ref(**table_or, &materialized_input);
    if (!materialized_or.ok()) {
        return materialized_or.status();
    }
    table_or = *materialized_or;
    auto column_or = optional_string_property(**object_or, "derivative", "column", "_value");
    if (!column_or.ok()) {
        return column_or.status();
    }
    auto time_column_or =
        optional_string_property(**object_or, "derivative", "timeColumn", "_time");
    if (!time_column_or.ok()) {
        return time_column_or.status();
    }
    auto non_negative_or = optional_bool_property(**object_or, "derivative", "nonNegative", false);
    if (!non_negative_or.ok()) {
        return non_negative_or.status();
    }
    auto initial_zero_or = optional_bool_property(**object_or, "derivative", "initialZero", false);
    if (!initial_zero_or.ok()) {
        return initial_zero_or.status();
    }

    int64_t unit_seconds = 1;
    if (const Value* unit_value = (*object_or)->lookup("unit"); unit_value != nullptr) {
        auto unit_or = parse_window_duration(*unit_value, "derivative", "unit");
        if (!unit_or.ok()) {
            return unit_or.status();
        }
        if (unit_or->kind != WindowDuration::Kind::FixedSeconds) {
            return absl::InvalidArgumentError(
                "derivative `unit` does not support calendar durations");
        }
        unit_seconds = unit_or->seconds;
    }

    std::unordered_map<std::string, Value> previous_value_by_group;
    std::unordered_map<std::string, int64_t> previous_time_by_group;
    std::vector<std::shared_ptr<ObjectValue>> rows;
    rows.reserve((*table_or)->rows.size());
    for (const auto& row : (*table_or)->rows) {
        if (row == nullptr) {
            continue;
        }
        const Value* current = row->lookup(*column_or);
        if (current == nullptr) {
            return absl::InvalidArgumentError(
                absl::StrCat("derivative requires `", *column_or, "` on every row"));
        }
        if (!is_numeric_value(*current)) {
            return absl::InvalidArgumentError(
                absl::StrCat("derivative `", *column_or, "` must be numeric"));
        }

        const Value* time_value = row->lookup(*time_column_or);
        if (time_value == nullptr) {
            return absl::InvalidArgumentError(
                absl::StrCat("derivative requires `", *time_column_or, "` on every row"));
        }

        std::string literal;
        if (time_value->type() == Value::Type::Time) {
            literal = time_value->as_time().literal;
        } else if (time_value->type() == Value::Type::String) {
            literal = time_value->as_string();
        } else {
            return absl::InvalidArgumentError(
                absl::StrCat("derivative `", *time_column_or, "` must be a time or string"));
        }
        auto seconds_or = parse_rfc3339_seconds(literal);
        if (!seconds_or.has_value()) {
            return absl::InvalidArgumentError(
                absl::StrCat("derivative could not parse RFC3339 time: ", literal));
        }

        const std::string group_key = group_key_for_row(*row);
        auto previous_value = previous_value_by_group.find(group_key);
        auto previous_time = previous_time_by_group.find(group_key);
        if (previous_value != previous_value_by_group.end() &&
            previous_time != previous_time_by_group.end()) {
            const int64_t delta_seconds = *seconds_or - previous_time->second;
            if (delta_seconds == 0) {
                return absl::InvalidArgumentError(
                    "derivative requires strictly increasing time within each group");
            }
            const double raw_delta =
                numeric_value(*current) - numeric_value(previous_value->second);
            Value rate = Value::null();
            if (!*non_negative_or || raw_delta >= 0.0) {
                rate = Value::floating(raw_delta * static_cast<double>(unit_seconds) /
                                       static_cast<double>(delta_seconds));
            } else if (*initial_zero_or) {
                rate = Value::floating(numeric_value(*current) * static_cast<double>(unit_seconds) /
                                       static_cast<double>(delta_seconds));
            }
            auto updated = object_with_upserted_property(*row, *column_or, std::move(rate));
            rows.push_back(std::make_shared<ObjectValue>(updated.as_object()));
        }
        previous_value_by_group[group_key] = *current;
        previous_time_by_group[group_key] = *seconds_or;
    }
    auto result = Value::table(
        (*table_or)->bucket, std::move(rows), (*table_or)->range_start, (*table_or)->range_stop);
    return with_materialization_barrier(std::move(result), **table_or, "derivative");
}

absl::StatusOr<Value> builtin_window(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "window");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "window", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    Value materialized_input;
    auto materialized_or = materialized_table_ref(**table_or, &materialized_input);
    if (!materialized_or.ok()) {
        return materialized_or.status();
    }
    table_or = *materialized_or;
    auto every_or = require_object_property(**object_or, "window", "every");
    if (!every_or.ok()) {
        return every_or.status();
    }
    auto parsed_every_or = parse_window_duration(**every_or, "window", "every");
    if (!parsed_every_or.ok()) {
        return parsed_every_or.status();
    }
    WindowDuration period = *parsed_every_or;
    if (const Value* period_value = (*object_or)->lookup("period"); period_value != nullptr) {
        auto parsed_period_or =
            parse_window_duration(*period_value, "window", "period", true, false);
        if (!parsed_period_or.ok()) {
            return parsed_period_or.status();
        }
        period = *parsed_period_or;
    }
    WindowDuration offset{.kind = WindowDuration::Kind::FixedSeconds, .seconds = 0, .months = 0};
    if (const Value* offset_value = (*object_or)->lookup("offset"); offset_value != nullptr) {
        auto parsed_offset_or =
            parse_window_duration(*offset_value, "window", "offset", true, true);
        if (!parsed_offset_or.ok()) {
            return parsed_offset_or.status();
        }
        offset = *parsed_offset_or;
    }
    auto create_empty_or = optional_bool_property(**object_or, "window", "createEmpty", false);
    if (!create_empty_or.ok()) {
        return create_empty_or.status();
    }
    auto time_column_or = optional_string_property(**object_or, "window", "timeColumn", "_time");
    if (!time_column_or.ok()) {
        return time_column_or.status();
    }
    auto start_column_or = optional_string_property(**object_or, "window", "startColumn", "_start");
    if (!start_column_or.ok()) {
        return start_column_or.status();
    }
    auto stop_column_or = optional_string_property(**object_or, "window", "stopColumn", "_stop");
    if (!stop_column_or.ok()) {
        return stop_column_or.status();
    }
    auto location_or = optional_window_location_property(**object_or, "window");
    if (!location_or.ok()) {
        return location_or.status();
    }
    const bool simple_tumbling_windows = period.kind == parsed_every_or->kind &&
                                         !window_duration_is_negative(period) &&
                                         ((period.kind == WindowDuration::Kind::FixedSeconds &&
                                           period.seconds == parsed_every_or->seconds) ||
                                          (period.kind == WindowDuration::Kind::CalendarMonths &&
                                           period.months == parsed_every_or->months));

    const auto table_range_start_seconds = (*table_or)->range_start.has_value()
                                               ? parse_rfc3339_seconds(*(*table_or)->range_start)
                                               : std::nullopt;
    const auto table_range_stop_seconds = (*table_or)->range_stop.has_value()
                                              ? parse_rfc3339_seconds(*(*table_or)->range_stop)
                                              : std::nullopt;

    std::vector<TableChunk> chunks;
    for (const auto& chunk : (*table_or)->tables) {
        std::unordered_map<int64_t, size_t> chunk_indexes;
        std::vector<TableChunk> chunk_windows;
        std::optional<int64_t> min_time_seconds;
        std::optional<int64_t> max_time_exclusive_seconds;

        for (const auto& row : chunk.rows) {
            if (row == nullptr) {
                continue;
            }
            const Value* time_value = row->lookup(*time_column_or);
            if (time_value == nullptr) {
                return absl::InvalidArgumentError(
                    absl::StrCat("window requires `", *time_column_or, "` on every row"));
            }
            std::string literal;
            if (time_value->type() == Value::Type::Time) {
                literal = time_value->as_time().literal;
            } else if (time_value->type() == Value::Type::String) {
                literal = time_value->as_string();
            } else {
                return absl::InvalidArgumentError(
                    absl::StrCat("window `", *time_column_or, "` must be a time or string"));
            }
            auto row_seconds = parse_rfc3339_seconds(literal);
            if (!row_seconds.has_value()) {
                return absl::InvalidArgumentError(
                    absl::StrCat("window could not parse RFC3339 time: ", literal));
            }

            if (!min_time_seconds.has_value() || *row_seconds < *min_time_seconds) {
                min_time_seconds = *row_seconds;
            }
            if (!max_time_exclusive_seconds.has_value() ||
                *row_seconds + 1 > *max_time_exclusive_seconds) {
                max_time_exclusive_seconds = *row_seconds + 1;
            }

            auto primary_start_or = aggregate_window_start_for_time(
                *row_seconds, *parsed_every_or, offset, *location_or);
            if (!primary_start_or.has_value()) {
                continue;
            }

            std::vector<int64_t> candidate_starts;
            if (simple_tumbling_windows) {
                candidate_starts.push_back(*primary_start_or);
            } else if (!window_duration_is_negative(period)) {
                for (std::optional<int64_t> current = primary_start_or; current.has_value();) {
                    auto bounds_or =
                        aggregate_window_bounds_for_start(*current, period, *location_or);
                    if (!bounds_or.has_value()) {
                        break;
                    }
                    if (aggregate_window_contains_time(*row_seconds, *bounds_or)) {
                        candidate_starts.push_back(*current);
                    }
                    if (bounds_or->upper_seconds <= *row_seconds) {
                        break;
                    }
                    auto previous_or = add_window_duration_to_time(
                        *current, negate_window_duration(*parsed_every_or), *location_or);
                    if (!previous_or.has_value() || *previous_or >= *current) {
                        break;
                    }
                    current = previous_or;
                }
            } else {
                auto current =
                    add_window_duration_to_time(*primary_start_or, *parsed_every_or, *location_or);
                while (current.has_value()) {
                    auto bounds_or =
                        aggregate_window_bounds_for_start(*current, period, *location_or);
                    if (!bounds_or.has_value()) {
                        break;
                    }
                    if (bounds_or->lower_seconds >= *row_seconds + 1) {
                        break;
                    }
                    if (aggregate_window_contains_time(*row_seconds, *bounds_or)) {
                        candidate_starts.push_back(*current);
                    }
                    auto next_or =
                        add_window_duration_to_time(*current, *parsed_every_or, *location_or);
                    if (!next_or.has_value() || *next_or <= *current) {
                        break;
                    }
                    current = next_or;
                }
            }

            for (const auto candidate_start : candidate_starts) {
                auto bounds_or =
                    aggregate_window_bounds_for_start(candidate_start, period, *location_or);
                if (!bounds_or.has_value()) {
                    continue;
                }
                const bool within_table_range =
                    !table_range_start_seconds.has_value() ||
                    !table_range_stop_seconds.has_value() ||
                    aggregate_window_is_within_range(
                        *bounds_or, *table_range_start_seconds, *table_range_stop_seconds);
                if (!within_table_range) {
                    continue;
                }
                auto [chunk_it, inserted] =
                    chunk_indexes.emplace(candidate_start, chunk_windows.size());
                if (inserted) {
                    TableChunk next;
                    next.group_key = window_group_object(chunk,
                                                         *start_column_or,
                                                         bounds_or->start_seconds,
                                                         *stop_column_or,
                                                         bounds_or->stop_seconds);
                    chunk_windows.push_back(std::move(next));
                }
                chunk_windows[chunk_it->second].rows.push_back(
                    row_with_window_bounds(*row,
                                           *start_column_or,
                                           bounds_or->start_seconds,
                                           *stop_column_or,
                                           bounds_or->stop_seconds,
                                           chunk_windows[chunk_it->second].group_key));
            }
        }

        if (*create_empty_or && min_time_seconds.has_value() &&
            max_time_exclusive_seconds.has_value()) {
            auto first_window_start_or = aggregate_window_start_for_time(
                *min_time_seconds, *parsed_every_or, offset, *location_or);
            if (first_window_start_or.has_value()) {
                if (!window_duration_is_negative(period)) {
                    while (true) {
                        auto previous_or =
                            add_window_duration_to_time(*first_window_start_or,
                                                        negate_window_duration(*parsed_every_or),
                                                        *location_or);
                        if (!previous_or.has_value() || *previous_or >= *first_window_start_or) {
                            break;
                        }
                        auto previous_bounds_or =
                            aggregate_window_bounds_for_start(*previous_or, period, *location_or);
                        if (!previous_bounds_or.has_value() ||
                            previous_bounds_or->upper_seconds <= *min_time_seconds) {
                            break;
                        }
                        first_window_start_or = previous_or;
                    }
                } else {
                    auto next_or = add_window_duration_to_time(
                        *first_window_start_or, *parsed_every_or, *location_or);
                    if (next_or.has_value() && *next_or > *first_window_start_or) {
                        first_window_start_or = next_or;
                    } else {
                        first_window_start_or = std::nullopt;
                    }
                }
            }

            for (auto window_start_or = first_window_start_or; window_start_or.has_value();) {
                auto bounds_or =
                    aggregate_window_bounds_for_start(*window_start_or, period, *location_or);
                if (!bounds_or.has_value()) {
                    break;
                }
                if (bounds_or->lower_seconds >= *max_time_exclusive_seconds) {
                    break;
                }
                const bool within_span = aggregate_window_intersects_range(
                    *bounds_or, *min_time_seconds, *max_time_exclusive_seconds);
                const bool within_table_range =
                    !table_range_start_seconds.has_value() ||
                    !table_range_stop_seconds.has_value() ||
                    aggregate_window_is_within_range(
                        *bounds_or, *table_range_start_seconds, *table_range_stop_seconds);
                if (within_span && within_table_range) {
                    auto [chunk_it, inserted] =
                        chunk_indexes.emplace(*window_start_or, chunk_windows.size());
                    if (inserted) {
                        TableChunk next;
                        next.group_key = window_group_object(chunk,
                                                             *start_column_or,
                                                             bounds_or->start_seconds,
                                                             *stop_column_or,
                                                             bounds_or->stop_seconds);
                        next.columns = visible_columns_in_chunk(chunk);
                        if (std::ranges::find(next.columns, *start_column_or) ==
                            next.columns.end()) {
                            next.columns.push_back(*start_column_or);
                        }
                        if (std::ranges::find(next.columns, *stop_column_or) ==
                            next.columns.end()) {
                            next.columns.push_back(*stop_column_or);
                        }
                        chunk_windows.push_back(std::move(next));
                    }
                }
                auto next_or =
                    add_window_duration_to_time(*window_start_or, *parsed_every_or, *location_or);
                if (!next_or.has_value() || *next_or <= *window_start_or) {
                    break;
                }
                window_start_or = next_or;
            }
        }

        std::ranges::stable_sort(chunk_windows, [&](const auto& lhs, const auto& rhs) {
            const Value* lhs_start =
                lhs.group_key != nullptr ? lhs.group_key->lookup(*start_column_or) : nullptr;
            const Value* rhs_start =
                rhs.group_key != nullptr ? rhs.group_key->lookup(*start_column_or) : nullptr;
            return compare_values(lhs_start, rhs_start) < 0;
        });
        chunks.insert(chunks.end(), chunk_windows.begin(), chunk_windows.end());
    }

    auto result = Value::table_stream((*table_or)->bucket,
                                      std::move(chunks),
                                      (*table_or)->range_start,
                                      (*table_or)->range_stop,
                                      (*table_or)->result_name);
    return with_materialization_barrier(std::move(result), **table_or, "window");
}

absl::StatusOr<Value> builtin_aggregate_window(const std::vector<Value>& args,
                                               const Value* default_location = nullptr) {
    auto object_or = require_object_argument(args, "aggregateWindow");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto table_or = require_table_property(**object_or, "aggregateWindow", "tables");
    if (!table_or.ok()) {
        return table_or.status();
    }
    Value materialized_input;
    auto materialized_or = materialized_table_ref(**table_or, &materialized_input);
    if (!materialized_or.ok()) {
        return materialized_or.status();
    }
    table_or = *materialized_or;
    auto every_value_or = require_object_property(**object_or, "aggregateWindow", "every");
    if (!every_value_or.ok()) {
        return every_value_or.status();
    }
    auto every_or = parse_window_duration(**every_value_or, "aggregateWindow", "every");
    if (!every_or.ok()) {
        return every_or.status();
    }
    auto location_or = optional_window_location_property(**object_or, "aggregateWindow");
    if (!location_or.ok()) {
        return location_or.status();
    }
    if ((*object_or)->lookup("location") == nullptr && default_location != nullptr) {
        auto fallback_or =
            parse_window_location_value(*default_location, "aggregateWindow", "option location");
        if (!fallback_or.ok()) {
            return fallback_or.status();
        }
        location_or = *fallback_or;
    }
    WindowDuration offset{
        .kind = WindowDuration::Kind::FixedSeconds,
        .seconds = 0,
        .months = 0,
    };
    if (const Value* offset_value = (*object_or)->lookup("offset"); offset_value != nullptr) {
        auto offset_or =
            parse_window_duration(*offset_value, "aggregateWindow", "offset", true, true);
        if (!offset_or.ok()) {
            return offset_or.status();
        }
        offset = *offset_or;
    }
    if (every_or->kind == WindowDuration::Kind::FixedSeconds &&
        offset.kind != WindowDuration::Kind::FixedSeconds) {
        return absl::InvalidArgumentError(
            "aggregateWindow fixed-duration windows do not support calendar `offset`");
    }
    WindowDuration period = *every_or;
    if (const Value* period_value = (*object_or)->lookup("period"); period_value != nullptr) {
        auto period_or =
            parse_window_duration(*period_value, "aggregateWindow", "period", true, false);
        if (!period_or.ok()) {
            return period_or.status();
        }
        period = *period_or;
    }
    if (every_or->kind == WindowDuration::Kind::FixedSeconds &&
        period.kind != WindowDuration::Kind::FixedSeconds) {
        return absl::InvalidArgumentError(
            "aggregateWindow fixed-duration windows do not support calendar `period`");
    }
    const bool simple_tumbling_windows =
        !window_duration_is_negative(period) && period.kind == every_or->kind &&
        period.seconds == every_or->seconds && period.months == every_or->months;
    auto fn_or = require_object_property(**object_or, "aggregateWindow", "fn");
    if (!fn_or.ok()) {
        return fn_or.status();
    }
    if ((*fn_or)->type() != Value::Type::Function) {
        return absl::InvalidArgumentError("aggregateWindow `fn` must be a function");
    }
    auto column_or = optional_string_property(**object_or, "aggregateWindow", "column", "_value");
    if (!column_or.ok()) {
        return column_or.status();
    }
    auto time_column_or =
        optional_string_property(**object_or, "aggregateWindow", "timeColumn", "_time");
    if (!time_column_or.ok()) {
        return time_column_or.status();
    }
    auto time_src_or = optional_string_property(**object_or, "aggregateWindow", "timeSrc", "_stop");
    if (!time_src_or.ok()) {
        return time_src_or.status();
    }
    auto time_dst_or = optional_string_property(**object_or, "aggregateWindow", "timeDst", "_time");
    if (!time_dst_or.ok()) {
        return time_dst_or.status();
    }
    auto create_empty_or =
        optional_bool_property(**object_or, "aggregateWindow", "createEmpty", true);
    if (!create_empty_or.ok()) {
        return create_empty_or.status();
    }
    std::optional<int64_t> table_range_start_seconds;
    std::optional<int64_t> table_range_stop_seconds;
    if ((*table_or)->range_start.has_value()) {
        table_range_start_seconds = parse_rfc3339_seconds(*(*table_or)->range_start);
    }
    if ((*table_or)->range_stop.has_value()) {
        table_range_stop_seconds = parse_rfc3339_seconds(*(*table_or)->range_stop);
    }
    std::vector<TableChunk> chunks;
    chunks.reserve((*table_or)->table_count());
    for (const auto& chunk : (*table_or)->tables) {
        struct ChunkSpan {
            std::shared_ptr<ObjectValue> template_row;
            int64_t min_time_seconds = 0;
            int64_t max_time_exclusive_seconds = 0;
            bool has_time = false;
        };
        struct GroupState {
            std::string group_key;
            std::vector<AggregateWindowBucket> buckets;
            std::unordered_map<int64_t, size_t> bucket_indexes;
            std::optional<size_t> timeless_bucket_index;
            ChunkSpan span;
        };

        std::vector<GroupState> groups;
        std::unordered_map<std::string, size_t> group_indexes;
        groups.reserve(chunk.rows.size());
        group_indexes.reserve(chunk.rows.size());

        for (const auto& row : chunk.rows) {
            if (row == nullptr) {
                continue;
            }
            const Value* aggregate_value = row->lookup(*column_or);
            if (aggregate_value == nullptr) {
                continue;
            }

            std::optional<int64_t> row_seconds;
            if (const Value* time_value = row->lookup(*time_column_or); time_value != nullptr) {
                std::optional<std::string> literal;
                if (time_value->type() == Value::Type::Time) {
                    literal = time_value->as_time().literal;
                } else if (time_value->type() == Value::Type::String) {
                    literal = time_value->as_string();
                }
                if (literal.has_value()) {
                    if (auto seconds = parse_rfc3339_seconds(*literal); seconds.has_value()) {
                        row_seconds = *seconds;
                    }
                }
            }

            const std::string group_key = group_key_for_row(*row);
            auto [group_it, inserted] = group_indexes.emplace(group_key, groups.size());
            if (inserted) {
                groups.push_back(GroupState{
                    .group_key = group_key,
                    .buckets = {},
                    .bucket_indexes = {},
                    .timeless_bucket_index = std::nullopt,
                    .span = {},
                });
                groups.back().buckets.reserve(chunk.rows.size());
                groups.back().bucket_indexes.reserve(chunk.rows.size());
            }
            auto& group = groups[group_it->second];

            if (*create_empty_or && row_seconds.has_value()) {
                if (!group.span.has_time) {
                    group.span.template_row =
                        aggregate_window_base_row(*row, *column_or, *time_dst_or);
                    group.span.min_time_seconds = table_range_start_seconds.has_value()
                                                      ? *table_range_start_seconds
                                                      : *row_seconds;
                    group.span.max_time_exclusive_seconds = table_range_stop_seconds.has_value()
                                                                ? *table_range_stop_seconds
                                                                : *row_seconds + 1;
                    group.span.has_time = true;
                } else {
                    if (!table_range_start_seconds.has_value() &&
                        *row_seconds < group.span.min_time_seconds) {
                        group.span.min_time_seconds = *row_seconds;
                    }
                    if (!table_range_stop_seconds.has_value() &&
                        *row_seconds + 1 > group.span.max_time_exclusive_seconds) {
                        group.span.max_time_exclusive_seconds = *row_seconds + 1;
                    }
                }
            }

            if (!row_seconds.has_value()) {
                if (!group.timeless_bucket_index.has_value()) {
                    group.timeless_bucket_index = group.buckets.size();
                    group.buckets.push_back(AggregateWindowBucket{
                        .start_seconds = std::nullopt,
                        .group_key = group_key,
                        .first_row = aggregate_window_base_row(*row, *column_or, *time_dst_or),
                        .values = {},
                    });
                }
                group.buckets[*group.timeless_bucket_index].values.push_back(*aggregate_value);
                continue;
            }

            auto primary_start_or =
                aggregate_window_start_for_time(*row_seconds, *every_or, offset, *location_or);
            if (!primary_start_or.has_value()) {
                continue;
            }
            if (simple_tumbling_windows) {
                auto bounds_or =
                    aggregate_window_bounds_for_start(*primary_start_or, period, *location_or);
                if (!bounds_or.has_value()) {
                    continue;
                }
                const bool within_table_range =
                    !table_range_start_seconds.has_value() ||
                    !table_range_stop_seconds.has_value() ||
                    aggregate_window_is_within_range(
                        *bounds_or, *table_range_start_seconds, *table_range_stop_seconds);
                if (within_table_range &&
                    aggregate_window_contains_time(*row_seconds, *bounds_or)) {
                    auto [bucket_it, bucket_inserted] =
                        group.bucket_indexes.emplace(*primary_start_or, group.buckets.size());
                    if (bucket_inserted) {
                        group.buckets.push_back(AggregateWindowBucket{
                            .start_seconds = *primary_start_or,
                            .group_key = group_key,
                            .first_row = aggregate_window_base_row(*row, *column_or, *time_dst_or),
                            .values = {},
                        });
                    }
                    group.buckets[bucket_it->second].values.push_back(*aggregate_value);
                }
                continue;
            }
            std::vector<int64_t> candidate_starts;
            if (!window_duration_is_negative(period)) {
                for (std::optional<int64_t> current = primary_start_or; current.has_value();) {
                    auto bounds_or =
                        aggregate_window_bounds_for_start(*current, period, *location_or);
                    if (!bounds_or.has_value()) {
                        break;
                    }
                    const bool within_table_range =
                        !table_range_start_seconds.has_value() ||
                        !table_range_stop_seconds.has_value() ||
                        aggregate_window_is_within_range(
                            *bounds_or, *table_range_start_seconds, *table_range_stop_seconds);
                    if (within_table_range &&
                        aggregate_window_contains_time(*row_seconds, *bounds_or)) {
                        candidate_starts.push_back(*current);
                    }
                    if (bounds_or->upper_seconds <= *row_seconds) {
                        break;
                    }
                    auto previous_or = add_window_duration_to_time(
                        *current, negate_window_duration(*every_or), *location_or);
                    if (!previous_or.has_value() || *previous_or >= *current) {
                        break;
                    }
                    current = previous_or;
                }
            } else {
                auto current =
                    add_window_duration_to_time(*primary_start_or, *every_or, *location_or);
                while (current.has_value()) {
                    auto bounds_or =
                        aggregate_window_bounds_for_start(*current, period, *location_or);
                    if (!bounds_or.has_value()) {
                        break;
                    }
                    if (bounds_or->lower_seconds >= *row_seconds + 1) {
                        break;
                    }
                    const bool within_table_range =
                        !table_range_start_seconds.has_value() ||
                        !table_range_stop_seconds.has_value() ||
                        aggregate_window_is_within_range(
                            *bounds_or, *table_range_start_seconds, *table_range_stop_seconds);
                    if (within_table_range &&
                        aggregate_window_contains_time(*row_seconds, *bounds_or)) {
                        candidate_starts.push_back(*current);
                    }
                    auto next_or = add_window_duration_to_time(*current, *every_or, *location_or);
                    if (!next_or.has_value() || *next_or <= *current) {
                        break;
                    }
                    current = next_or;
                }
            }

            for (const auto candidate_start : candidate_starts) {
                auto [bucket_it, bucket_inserted] =
                    group.bucket_indexes.emplace(candidate_start, group.buckets.size());
                if (bucket_inserted) {
                    group.buckets.push_back(AggregateWindowBucket{
                        .start_seconds = candidate_start,
                        .group_key = group_key,
                        .first_row = aggregate_window_base_row(*row, *column_or, *time_dst_or),
                        .values = {},
                    });
                }
                group.buckets[bucket_it->second].values.push_back(*aggregate_value);
            }
        }

        for (auto& group : groups) {
            if (*create_empty_or && group.span.template_row != nullptr &&
                group.span.max_time_exclusive_seconds > group.span.min_time_seconds) {
                auto first_window_start_or = aggregate_window_start_for_time(
                    group.span.min_time_seconds, *every_or, offset, *location_or);
                if (first_window_start_or.has_value()) {
                    if (!window_duration_is_negative(period)) {
                        while (true) {
                            auto previous_or =
                                add_window_duration_to_time(*first_window_start_or,
                                                            negate_window_duration(*every_or),
                                                            *location_or);
                            if (!previous_or.has_value() ||
                                *previous_or >= *first_window_start_or) {
                                break;
                            }
                            auto previous_bounds_or = aggregate_window_bounds_for_start(
                                *previous_or, period, *location_or);
                            if (!previous_bounds_or.has_value() ||
                                previous_bounds_or->upper_seconds <= group.span.min_time_seconds) {
                                break;
                            }
                            first_window_start_or = previous_or;
                        }
                    } else {
                        auto next_or = add_window_duration_to_time(
                            *first_window_start_or, *every_or, *location_or);
                        if (next_or.has_value() && *next_or > *first_window_start_or) {
                            first_window_start_or = next_or;
                        } else {
                            first_window_start_or = std::nullopt;
                        }
                    }
                }

                for (auto window_start_or = first_window_start_or; window_start_or.has_value();) {
                    auto bounds_or =
                        aggregate_window_bounds_for_start(*window_start_or, period, *location_or);
                    if (!bounds_or.has_value()) {
                        break;
                    }
                    if (bounds_or->lower_seconds >= group.span.max_time_exclusive_seconds) {
                        break;
                    }
                    if (aggregate_window_intersects_range(*bounds_or,
                                                          group.span.min_time_seconds,
                                                          group.span.max_time_exclusive_seconds) &&
                        (!table_range_start_seconds.has_value() ||
                         !table_range_stop_seconds.has_value() ||
                         aggregate_window_is_within_range(
                             *bounds_or, *table_range_start_seconds, *table_range_stop_seconds))) {
                        auto [it, inserted] =
                            group.bucket_indexes.emplace(*window_start_or, group.buckets.size());
                        if (inserted) {
                            group.buckets.push_back(AggregateWindowBucket{
                                .start_seconds = *window_start_or,
                                .group_key = group.group_key,
                                .first_row = clone_row(*group.span.template_row),
                                .values = {},
                            });
                        }
                    }
                    auto next_or =
                        add_window_duration_to_time(*window_start_or, *every_or, *location_or);
                    if (!next_or.has_value() || *next_or <= *window_start_or) {
                        break;
                    }
                    window_start_or = next_or;
                }
            }

            std::ranges::stable_sort(group.buckets, [](const auto& lhs, const auto& rhs) {
                if (lhs.start_seconds == rhs.start_seconds) {
                    return false;
                }
                if (!lhs.start_seconds.has_value()) {
                    return false;
                }
                if (!rhs.start_seconds.has_value()) {
                    return true;
                }
                return *lhs.start_seconds < *rhs.start_seconds;
            });

            TableChunk output_chunk;
            output_chunk.rows.reserve(group.buckets.size());
            for (const auto& bucket : group.buckets) {
                if (bucket.first_row == nullptr) {
                    continue;
                }
                Value aggregate_value = Value::null();
                if (bucket.values.empty()) {
                    if (!*create_empty_or ||
                        aggregate_window_fn_drops_empty((*fn_or)->as_function())) {
                        continue;
                    }
                    aggregate_value = empty_window_aggregate_value((*fn_or)->as_function());
                } else {
                    auto aggregate_or =
                        invoke_window_aggregate((*fn_or)->as_function(), bucket.values);
                    if (!aggregate_or.ok()) {
                        return aggregate_or.status();
                    }
                    aggregate_value = *aggregate_or;
                }

                std::optional<int64_t> window_start_seconds;
                std::optional<int64_t> window_stop_seconds;
                std::optional<int64_t> time_src_seconds;
                if (bucket.start_seconds.has_value()) {
                    auto bounds_or = aggregate_window_bounds_for_start(
                        *bucket.start_seconds, period, *location_or);
                    if (!bounds_or.has_value()) {
                        return absl::InternalError("aggregateWindow failed to compute window stop");
                    }
                    window_start_seconds = *bucket.start_seconds;
                    window_stop_seconds = bounds_or->stop_seconds;
                    time_src_seconds = aggregate_window_time_src_seconds(bucket.first_row,
                                                                         *bucket.start_seconds,
                                                                         *window_stop_seconds,
                                                                         *time_src_or);
                }
                output_chunk.rows.push_back(aggregate_window_output_row(bucket.first_row,
                                                                        *column_or,
                                                                        std::move(aggregate_value),
                                                                        window_start_seconds,
                                                                        window_stop_seconds,
                                                                        *time_dst_or,
                                                                        time_src_seconds));
            }
            chunks.push_back(std::move(output_chunk));
        }
    }
    if (chunks.empty()) {
        chunks.emplace_back();
    }
    auto result = table_with_chunks_like(**table_or, std::move(chunks));
    return with_materialization_barrier(std::move(result), **table_or, "aggregateWindow");
}

} // namespace

bool InstallKnownUniverseWindowBuiltin(Environment& env, const std::string& name) {
    if (name == "elapsed") {
        install_builtin(env, "elapsed", builtin_elapsed, "tables");
        return true;
    }
    if (name == "difference") {
        install_builtin(env, "difference", builtin_difference, "tables");
        return true;
    }
    if (name == "derivative") {
        install_builtin(env, "derivative", builtin_derivative, "tables");
        return true;
    }
    if (name == "window") {
        install_builtin(env, "window", builtin_window, "tables");
        return true;
    }
    if (name == "aggregateWindow") {
        install_builtin(
            env,
            "aggregateWindow",
            [&env](const std::vector<Value>& args) -> absl::StatusOr<Value> {
                auto option_or = env.lookup_option("location");
                if (option_or.ok()) {
                    return builtin_aggregate_window(args, &*option_or);
                }
                if (option_or.status().code() != absl::StatusCode::kNotFound) {
                    return option_or.status();
                }
                return builtin_aggregate_window(args);
            },
            "tables");
        return true;
    }
    return false;
}

} // namespace pl::flux
