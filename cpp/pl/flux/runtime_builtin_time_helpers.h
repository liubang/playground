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

#include "cpp/pl/flux/runtime_builtin_table_helpers.h"
#include "absl/time/civil_time.h"
#include "absl/time/time.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>

namespace pl {

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif

namespace {

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

std::optional<int64_t> parse_rfc3339_seconds(const std::string& literal);
std::string format_rfc3339_seconds(int64_t seconds);

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
    const auto row_seconds = parse_rfc3339_seconds(*row_time);
    const auto start_seconds = start.has_value() ? parse_rfc3339_seconds(*start) : std::nullopt;
    const auto stop_seconds = stop.has_value() ? parse_rfc3339_seconds(*stop) : std::nullopt;
    if (row_seconds.has_value() && (!start.has_value() || start_seconds.has_value()) &&
        (!stop.has_value() || stop_seconds.has_value())) {
        if (start_seconds.has_value() && *row_seconds < *start_seconds) {
            return false;
        }
        if (stop_seconds.has_value() && *row_seconds >= *stop_seconds) {
            return false;
        }
        return true;
    }
    if (start.has_value() && *row_time < *start) {
        return false;
    }
    if (stop.has_value() && *row_time >= *stop) {
        return false;
    }
    return true;
}

bool parse_fixed_int(const std::string& text, size_t offset, size_t width, int* out) {
    if (offset + width > text.size()) {
        return false;
    }
    int value = 0;
    for (size_t i = 0; i < width; ++i) {
        const auto ch = text[offset + i];
        if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
            return false;
        }
        value = value * 10 + (ch - '0');
    }
    *out = value;
    return true;
}

int64_t days_from_civil(int year, unsigned month, unsigned day) {
    year -= static_cast<int>(month <= 2);
    const int era = (year >= 0 ? year : year - 399) / 400;
    const auto yoe = static_cast<unsigned>(year - era * 400);
    const auto shifted_month =
        static_cast<unsigned>(static_cast<int>(month) + (month > 2 ? -3 : 9));
    const unsigned doy = (153 * shifted_month + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int>(doe) - 719468;
}

std::optional<int64_t> parse_rfc3339_seconds(const std::string& literal) {
    absl::Time timestamp;
    std::string error;
    if (!absl::ParseTime(absl::RFC3339_full, literal, &timestamp, &error)) {
        return std::nullopt;
    }
    return absl::ToUnixSeconds(timestamp);
}

std::string format_rfc3339_seconds(int64_t seconds) {
    return absl::FormatTime("%Y-%m-%dT%H:%M:%SZ", absl::FromUnixSeconds(seconds),
                            absl::UTCTimeZone());
}

struct WindowDuration {
    enum class Kind {
        FixedSeconds,
        CalendarMonths,
    };

    Kind kind = Kind::FixedSeconds;
    int64_t seconds = 0;
    int64_t months = 0;
};

absl::StatusOr<WindowDuration> parse_window_duration(const Value& value,
                                                     const std::string& name,
                                                     const std::string& property,
                                                     bool allow_negative,
                                                     bool allow_zero);

struct WindowLocation {
    enum class Kind {
        Utc,
        FixedOffset,
        NamedZone,
    };

    Kind kind = Kind::Utc;
    int64_t fixed_offset_seconds = 0;
    std::string zone_name = "UTC";
    absl::TimeZone zone = absl::UTCTimeZone();
};

int64_t floor_div(int64_t lhs, int64_t rhs);

int64_t utc_seconds_from_civil(int year,
                               unsigned month,
                               unsigned day,
                               unsigned hour = 0,
                               unsigned minute = 0,
                               unsigned second = 0) {
    return days_from_civil(year, month, day) * 24 * 60 * 60 + static_cast<int64_t>(hour) * 60 * 60 +
           static_cast<int64_t>(minute) * 60 + static_cast<int64_t>(second);
}

absl::CivilSecond civil_second_from_utc_seconds(int64_t seconds) {
    return absl::ToCivilSecond(absl::FromUnixSeconds(seconds), absl::UTCTimeZone());
}

int64_t seconds_from_civil_second(const absl::CivilSecond& civil) {
    return utc_seconds_from_civil(
        static_cast<int>(civil.year()), static_cast<unsigned>(civil.month()),
        static_cast<unsigned>(civil.day()), static_cast<unsigned>(civil.hour()),
        static_cast<unsigned>(civil.minute()), static_cast<unsigned>(civil.second()));
}

absl::CivilSecond civil_second_in_location(int64_t seconds, const WindowLocation& location) {
    if (location.kind == WindowLocation::Kind::NamedZone) {
        return absl::ToCivilSecond(absl::FromUnixSeconds(seconds), location.zone);
    }
    return civil_second_from_utc_seconds(seconds + location.fixed_offset_seconds);
}

int64_t seconds_for_civil_in_location(const absl::CivilSecond& civil,
                                      const WindowLocation& location) {
    if (location.kind == WindowLocation::Kind::NamedZone) {
        const auto info = location.zone.At(civil);
        if (info.kind == absl::TimeZone::TimeInfo::SKIPPED) {
            return absl::ToUnixSeconds(info.trans);
        }
        return absl::ToUnixSeconds(info.pre);
    }
    return seconds_from_civil_second(civil) - location.fixed_offset_seconds;
}

int64_t month_index_for_seconds(int64_t seconds) {
    auto time = static_cast<std::time_t>(seconds);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    return static_cast<int64_t>(tm.tm_year + 1900) * 12 + tm.tm_mon;
}

int64_t seconds_for_month_index(int64_t month_index) {
    int64_t year = floor_div(month_index, 12);
    int64_t month = month_index - year * 12;
    return utc_seconds_from_civil(static_cast<int>(year), static_cast<unsigned>(month + 1), 1);
}

int64_t advance_calendar_months(int64_t seconds, int64_t months) {
    return seconds_for_month_index(month_index_for_seconds(seconds) + months);
}

absl::CivilSecond add_months_to_civil_second(const absl::CivilSecond& civil, int64_t months) {
    const int64_t month_index =
        static_cast<int64_t>(civil.year()) * 12 + static_cast<int64_t>(civil.month()) - 1 + months;
    const int64_t year = floor_div(month_index, 12);
    const int64_t month = month_index - year * 12;
    return absl::CivilSecond(static_cast<int>(year), static_cast<int>(month + 1), civil.day(),
                             civil.hour(), civil.minute(), civil.second());
}

absl::StatusOr<WindowLocation> parse_window_location_value(const Value& value,
                                                           const std::string& name,
                                                           const std::string& property) {
    if (value.type() != Value::Type::Object) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " `", property, "` must be an object record"));
    }
    const auto& location = value.as_object();
    auto zone_or = optional_string_property(location, name, "zone", "UTC");
    if (!zone_or.ok()) {
        return zone_or.status();
    }
    int64_t fixed_offset_seconds = 0;
    if (const Value* offset_value = location.lookup("offset"); offset_value != nullptr) {
        auto offset_or = parse_window_duration(*offset_value, name, "location.offset", true, true);
        if (!offset_or.ok()) {
            return offset_or.status();
        }
        if (offset_or->kind != WindowDuration::Kind::FixedSeconds) {
            return absl::InvalidArgumentError(
                absl::StrCat(name, " `location.offset` does not support calendar durations"));
        }
        fixed_offset_seconds = offset_or->seconds;
    }
    if (*zone_or == "UTC") {
        return WindowLocation{.kind = fixed_offset_seconds == 0 ? WindowLocation::Kind::Utc
                                                                : WindowLocation::Kind::FixedOffset,
                              .fixed_offset_seconds = fixed_offset_seconds,
                              .zone_name = *zone_or,
                              .zone = absl::UTCTimeZone()};
    }
    if (fixed_offset_seconds != 0) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " `", property, "` with a named zone must use offset 0s"));
    }
    absl::TimeZone zone;
    if (!absl::LoadTimeZone(*zone_or, &zone)) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " unknown timezone location: ", *zone_or));
    }
    return WindowLocation{.kind = WindowLocation::Kind::NamedZone,
                          .fixed_offset_seconds = 0,
                          .zone_name = *zone_or,
                          .zone = zone};
}

absl::StatusOr<WindowLocation> optional_window_location_property(const ObjectValue& object,
                                                                 const std::string& name) {
    const Value* location_value = object.lookup("location");
    if (location_value == nullptr) {
        return WindowLocation{};
    }
    return parse_window_location_value(*location_value, name, "location");
}

absl::StatusOr<WindowDuration> parse_window_duration(const Value& value,
                                                     const std::string& name,
                                                     const std::string& property,
                                                     bool allow_negative = false,
                                                     bool allow_zero = false) {
    std::string literal;
    if (value.type() == Value::Type::Duration) {
        literal = value.as_duration().literal;
    } else if (value.type() == Value::Type::String) {
        literal = value.as_string();
    } else {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " `", property, "` must be a duration"));
    }

    int sign = 1;
    size_t index = 0;
    if (!literal.empty() && (literal[0] == '+' || literal[0] == '-')) {
        if (!allow_negative) {
            return absl::InvalidArgumentError(
                absl::StrCat(name, " `", property, "` must be a positive duration"));
        }
        sign = literal[0] == '-' ? -1 : 1;
        index = 1;
    }

    if (index >= literal.size()) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " `", property, "` must be a duration"));
    }
    int64_t fixed_total = 0;
    int64_t calendar_months = 0;
    bool saw_fixed_unit = false;
    bool saw_calendar_unit = false;
    while (index < literal.size()) {
        if (std::isdigit(static_cast<unsigned char>(literal[index])) == 0) {
            return absl::InvalidArgumentError(
                absl::StrCat(name, " `", property, "` must be ",
                             allow_negative ? "a duration" : "a positive duration"));
        }
        int64_t amount = 0;
        while (index < literal.size() &&
               (std::isdigit(static_cast<unsigned char>(literal[index])) != 0)) {
            amount = amount * 10 + (literal[index] - '0');
            ++index;
        }
        const size_t unit_begin = index;
        while (index < literal.size() &&
               (std::isalpha(static_cast<unsigned char>(literal[index])) != 0)) {
            ++index;
        }
        const auto unit = literal.substr(unit_begin, index - unit_begin);
        if (unit == "s") {
            fixed_total += amount;
            saw_fixed_unit = true;
        } else if (unit == "m") {
            fixed_total += amount * 60;
            saw_fixed_unit = true;
        } else if (unit == "h") {
            fixed_total += amount * 60 * 60;
            saw_fixed_unit = true;
        } else if (unit == "d") {
            fixed_total += amount * 24 * 60 * 60;
            saw_fixed_unit = true;
        } else if (unit == "w") {
            fixed_total += amount * 7 * 24 * 60 * 60;
            saw_fixed_unit = true;
        } else if (unit == "mo") {
            calendar_months += amount;
            saw_calendar_unit = true;
        } else if (unit == "y") {
            calendar_months += amount * 12;
            saw_calendar_unit = true;
        } else {
            return absl::InvalidArgumentError(
                absl::StrCat(name, " `", property, "` supports s, m, h, d, w, mo, and y units"));
        }
        if (saw_fixed_unit && saw_calendar_unit) {
            return absl::InvalidArgumentError(absl::StrCat(
                name, " `", property, "` cannot mix calendar units with fixed-duration units"));
        }
    }
    fixed_total *= sign;
    calendar_months *= sign;
    if ((!allow_zero && fixed_total == 0 && calendar_months == 0) ||
        (!allow_negative && fixed_total < 0)) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " `", property, "` must be a positive duration"));
    }
    if (!allow_negative && fixed_total <= 0 && calendar_months == 0) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " `", property, "` must be a positive duration"));
    }
    if (saw_calendar_unit) {
        if ((!allow_zero && calendar_months == 0) || (!allow_negative && calendar_months < 0)) {
            return absl::InvalidArgumentError(
                absl::StrCat(name, " `", property, "` must be a positive duration"));
        }
        return WindowDuration{
            .kind = WindowDuration::Kind::CalendarMonths, .seconds = 0, .months = calendar_months};
    }
    return WindowDuration{
        .kind = WindowDuration::Kind::FixedSeconds, .seconds = fixed_total, .months = 0};
}

int64_t floor_div(int64_t lhs, int64_t rhs) {
    int64_t quotient = lhs / rhs;
    int64_t remainder = lhs % rhs;
    if (remainder != 0 && ((remainder > 0) != (rhs > 0))) {
        --quotient;
    }
    return quotient;
}

bool window_duration_is_negative(const WindowDuration& duration) {
    return duration.kind == WindowDuration::Kind::FixedSeconds ? duration.seconds < 0
                                                               : duration.months < 0;
}

bool window_duration_is_zero(const WindowDuration& duration) {
    return duration.kind == WindowDuration::Kind::FixedSeconds ? duration.seconds == 0
                                                               : duration.months == 0;
}

WindowDuration negate_window_duration(const WindowDuration& duration) {
    if (duration.kind == WindowDuration::Kind::FixedSeconds) {
        return WindowDuration{
            .kind = WindowDuration::Kind::FixedSeconds, .seconds = -duration.seconds, .months = 0};
    }
    return WindowDuration{
        .kind = WindowDuration::Kind::CalendarMonths, .seconds = 0, .months = -duration.months};
}

std::optional<int64_t> add_window_duration_to_time(int64_t seconds,
                                                   const WindowDuration& duration,
                                                   const WindowLocation& location) {
    if (duration.kind == WindowDuration::Kind::FixedSeconds) {
        if (location.kind == WindowLocation::Kind::Utc ||
            location.kind == WindowLocation::Kind::FixedOffset) {
            return seconds + duration.seconds;
        }
        const auto local = civil_second_in_location(seconds, location);
        return seconds_for_civil_in_location(
            civil_second_from_utc_seconds(seconds_from_civil_second(local) + duration.seconds),
            location);
    }
    const auto local = civil_second_in_location(seconds, location);
    return seconds_for_civil_in_location(add_months_to_civil_second(local, duration.months),
                                         location);
}

std::optional<int64_t> aggregate_window_start_for_time(int64_t seconds,
                                                       const WindowDuration& every,
                                                       const WindowDuration& offset,
                                                       const WindowLocation& location) {
    if (every.kind == WindowDuration::Kind::FixedSeconds) {
        if (offset.kind != WindowDuration::Kind::FixedSeconds) {
            return std::nullopt;
        }
        const int64_t offset_seconds = offset.seconds;
        if (location.kind == WindowLocation::Kind::Utc ||
            location.kind == WindowLocation::Kind::FixedOffset) {
            const int64_t anchor = offset_seconds - location.fixed_offset_seconds;
            return floor_div(seconds - anchor, every.seconds) * every.seconds + anchor;
        }
        const auto local = civil_second_in_location(seconds, location);
        const int64_t local_seconds = seconds_from_civil_second(local);
        const int64_t start_local_seconds =
            floor_div(local_seconds - offset_seconds, every.seconds) * every.seconds +
            offset_seconds;
        return seconds_for_civil_in_location(civil_second_from_utc_seconds(start_local_seconds),
                                             location);
    }
    if (window_duration_is_zero(offset)) {
        const auto local = civil_second_in_location(seconds, location);
        const int64_t month_index =
            static_cast<int64_t>(local.year()) * 12 + static_cast<int64_t>(local.month()) - 1;
        const int64_t start_index = floor_div(month_index, every.months) * every.months;
        const int64_t year = floor_div(start_index, 12);
        const int64_t month = start_index - year * 12;
        return seconds_for_civil_in_location(
            absl::CivilSecond(static_cast<int>(year), static_cast<int>(month + 1), 1, 0, 0, 0),
            location);
    }
    auto anchor_or = add_window_duration_to_time(0, offset, location);
    if (!anchor_or.has_value()) {
        return std::nullopt;
    }
    const auto local = civil_second_in_location(seconds, location);
    const auto anchor_local = civil_second_in_location(*anchor_or, location);
    const int64_t month_index =
        static_cast<int64_t>(local.year()) * 12 + static_cast<int64_t>(local.month()) - 1;
    const int64_t anchor_month_index = static_cast<int64_t>(anchor_local.year()) * 12 +
                                       static_cast<int64_t>(anchor_local.month()) - 1;
    int64_t step = floor_div(month_index - anchor_month_index, every.months);
    auto candidate_or =
        add_window_duration_to_time(*anchor_or,
                                    WindowDuration{.kind = WindowDuration::Kind::CalendarMonths,
                                                   .seconds = 0,
                                                   .months = step * every.months},
                                    location);
    if (!candidate_or.has_value()) {
        return std::nullopt;
    }
    while (*candidate_or > seconds) {
        auto previous_or =
            add_window_duration_to_time(*candidate_or, negate_window_duration(every), location);
        if (!previous_or.has_value() || *previous_or >= *candidate_or) {
            break;
        }
        candidate_or = previous_or;
    }
    while (true) {
        auto next_or = add_window_duration_to_time(*candidate_or, every, location);
        if (!next_or.has_value() || *next_or <= *candidate_or || *next_or > seconds) {
            break;
        }
        candidate_or = next_or;
    }
    return candidate_or;
}

struct WindowBounds {
    int64_t start_seconds = 0;
    int64_t stop_seconds = 0;
    int64_t lower_seconds = 0;
    int64_t upper_seconds = 0;
};

std::optional<WindowBounds> aggregate_window_bounds_for_start(int64_t start_seconds,
                                                              const WindowDuration& period,
                                                              const WindowLocation& location) {
    auto stop_or = add_window_duration_to_time(start_seconds, period, location);
    if (!stop_or.has_value()) {
        return std::nullopt;
    }
    return WindowBounds{.start_seconds = start_seconds,
                        .stop_seconds = *stop_or,
                        .lower_seconds = std::min(start_seconds, *stop_or),
                        .upper_seconds = std::max(start_seconds, *stop_or)};
}

bool aggregate_window_contains_time(int64_t seconds, const WindowBounds& bounds) {
    return seconds >= bounds.lower_seconds && seconds < bounds.upper_seconds;
}

bool aggregate_window_intersects_range(const WindowBounds& bounds,
                                       int64_t range_start_seconds,
                                       int64_t range_stop_seconds) {
    return bounds.upper_seconds > range_start_seconds && bounds.lower_seconds < range_stop_seconds;
}

bool aggregate_window_is_within_range(const WindowBounds& bounds,
                                      int64_t range_start_seconds,
                                      int64_t range_stop_seconds) {
    return bounds.lower_seconds >= range_start_seconds &&
           bounds.upper_seconds <= range_stop_seconds;
}

bool aggregate_window_fn_drops_empty(const FunctionValue& fn) {
    if (fn.kind != FunctionValue::Kind::Builtin) {
        return false;
    }
    return fn.name == "first" || fn.name == "last";
}


} // namespace

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

} // namespace pl
