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
// Created: 2026/04/24 22:10

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/civil_time.h"
#include "absl/time/time.h"
#include "cpp/pl/flux/common/compat.h"
#include "cpp/pl/flux/runtime/runtime_builtin_package.h"
#include "cpp/pl/flux/syntax/strconv.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <regex>
#include <simdjson.h>
#include <string>
#include <vector>

namespace pl::flux::builtin {
namespace {

struct ParsedDuration {
    int64_t seconds = 0;
    int64_t months = 0;
};

Value make_builtin_value(const std::string& name,
                         FunctionValue::BuiltinCallback fn,
                         std::string pipe_param_name = {}) {
    auto callable = std::make_shared<FunctionValue>();
    callable->kind = FunctionValue::Kind::Builtin;
    callable->name = name;
    callable->pipe_param_name = std::move(pipe_param_name);
    callable->builtin = std::move(fn);
    return Value::function(std::move(callable));
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
        return absl::InvalidArgumentError(absl::StrCat(object_name, " requires `", property, "`"));
    }
    return value;
}

absl::StatusOr<std::string> string_property(const ObjectValue& object,
                                            const std::string& name,
                                            const std::string& property) {
    auto value_or = require_object_property(object, name, property);
    if (!value_or.ok()) {
        return value_or.status();
    }
    if ((*value_or)->type() != Value::Type::String) {
        return absl::InvalidArgumentError(absl::StrCat(name, " `", property, "` must be a string"));
    }
    return (*value_or)->as_string();
}

absl::StatusOr<std::string> optional_string_value(const std::vector<Value>& args,
                                                  const std::string& name) {
    if (args.size() != 1) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " expects exactly one string argument"));
    }
    if (args[0].type() == Value::Type::String) {
        return args[0].as_string();
    }
    if (args[0].type() == Value::Type::Object) {
        return string_property(args[0].as_object(), name, "v");
    }
    return absl::InvalidArgumentError(absl::StrCat(name, " expects a string argument"));
}

absl::StatusOr<double> number_property(const ObjectValue& object,
                                       const std::string& name,
                                       const std::string& property) {
    auto value_or = require_object_property(object, name, property);
    if (!value_or.ok()) {
        return value_or.status();
    }
    switch ((*value_or)->type()) {
        case Value::Type::Float:
            return (*value_or)->as_float();
        case Value::Type::Int:
            return static_cast<double>((*value_or)->as_int());
        case Value::Type::UInt:
            return static_cast<double>((*value_or)->as_uint());
        default:
            return absl::InvalidArgumentError(
                absl::StrCat(name, " `", property, "` must be numeric"));
    }
}

absl::StatusOr<double> number_argument(const std::vector<Value>& args, const std::string& name) {
    if (args.size() != 1) {
        return absl::InvalidArgumentError(absl::StrCat(name, " expects exactly one argument"));
    }
    const Value* value = &args[0];
    if (args[0].type() == Value::Type::Object) {
        auto value_or = require_object_property(args[0].as_object(), name, "x");
        if (!value_or.ok()) {
            return value_or.status();
        }
        value = *value_or;
    }
    switch (value->type()) {
        case Value::Type::Float:
            return value->as_float();
        case Value::Type::Int:
            return static_cast<double>(value->as_int());
        case Value::Type::UInt:
            return static_cast<double>(value->as_uint());
        default:
            return absl::InvalidArgumentError(absl::StrCat(name, " expects a numeric argument"));
    }
}

std::string value_type_name(const Value& value) {
    switch (value.type()) {
        case Value::Type::Null:
            return "null";
        case Value::Type::Bool:
            return "bool";
        case Value::Type::Int:
            return "int";
        case Value::Type::UInt:
            return "uint";
        case Value::Type::Float:
            return "float";
        case Value::Type::String:
            return "string";
        case Value::Type::Duration:
            return "duration";
        case Value::Type::Time:
            return "time";
        case Value::Type::Regex:
            return "regexp";
        case Value::Type::Array:
            return "array";
        case Value::Type::Object:
            return "object";
        case Value::Type::Table:
            return "table";
        case Value::Type::Function:
            return "function";
        default:
            PL_FLUX_UNREACHABLE();
    }
}

absl::StatusOr<std::string> dict_key_string(const Value& value, const std::string& name) {
    switch (value.type()) {
        case Value::Type::Bool:
        case Value::Type::Int:
        case Value::Type::UInt:
        case Value::Type::Float:
        case Value::Type::Duration:
        case Value::Type::Time:
        case Value::Type::Regex:
            return value.string();
        case Value::Type::String:
            return value.as_string();
        case Value::Type::Null:
        case Value::Type::Array:
        case Value::Type::Object:
        case Value::Type::Table:
        case Value::Type::Function:
            return absl::InvalidArgumentError(
                absl::StrCat(name, " `key` must be a comparable scalar"));
        default:
            PL_FLUX_UNREACHABLE();
    }
}

absl::StatusOr<const ObjectValue*> dict_property(const ObjectValue& object,
                                                 const std::string& name) {
    auto value_or = require_object_property(object, name, "dict");
    if (!value_or.ok()) {
        return value_or.status();
    }
    if ((*value_or)->type() != Value::Type::Object) {
        return absl::InvalidArgumentError(absl::StrCat(name, " `dict` must be a dictionary"));
    }
    return &(*value_or)->as_object();
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

int64_t utc_seconds_from_civil(int year,
                               unsigned month,
                               unsigned day,
                               unsigned hour = 0,
                               unsigned minute = 0,
                               unsigned second = 0) {
    return days_from_civil(year, month, day) * 24 * 60 * 60 + static_cast<int64_t>(hour) * 60 * 60 +
           static_cast<int64_t>(minute) * 60 + static_cast<int64_t>(second);
}

int64_t weekday_sunday_zero(const absl::CivilSecond& civil) {
    const int64_t days =
        days_from_civil(static_cast<int>(civil.year()), static_cast<unsigned>(civil.month()),
                        static_cast<unsigned>(civil.day()));
    return ((days + 4) % 7 + 7) % 7;
}

absl::StatusOr<std::string> duration_literal_property(const ObjectValue& object,
                                                      const std::string& name,
                                                      const std::string& property) {
    auto value_or = require_object_property(object, name, property);
    if (!value_or.ok()) {
        return value_or.status();
    }
    if ((*value_or)->type() != Value::Type::Duration) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " `", property, "` must be a duration"));
    }
    return (*value_or)->as_duration().literal;
}

absl::StatusOr<std::string> time_literal_property(const ObjectValue& object,
                                                  const std::string& name,
                                                  const std::string& property) {
    auto value_or = require_object_property(object, name, property);
    if (!value_or.ok()) {
        return value_or.status();
    }
    if ((*value_or)->type() == Value::Type::Time) {
        return (*value_or)->as_time().literal;
    }
    if ((*value_or)->type() == Value::Type::String) {
        return (*value_or)->as_string();
    }
    return absl::InvalidArgumentError(
        absl::StrCat(name, " `", property, "` must be a time or string"));
}

absl::StatusOr<ParsedDuration> parse_duration_literal(const std::string& literal,
                                                      const std::string& name,
                                                      const std::string& property,
                                                      bool allow_negative = true) {
    int64_t sign = 1;
    std::string normalized = literal;
    if (!normalized.empty() && normalized.front() == '-') {
        sign = -1;
        normalized.erase(normalized.begin());
    }
    if (normalized.empty()) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " `", property, "` must be a duration"));
    }
    if (sign < 0 && !allow_negative) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " `", property, "` must be a positive duration"));
    }
    auto parts_or = StrConv::parse_duration(normalized);
    if (!parts_or.ok()) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " failed to parse `", property, "` duration"));
    }

    ParsedDuration parsed;
    for (const auto& part : *parts_or) {
        const int64_t magnitude = sign * part->magnitude;
        if (part->unit == "ns" || part->unit == "us" || part->unit == "ms") {
            continue;
        }
        if (part->unit == "s") {
            parsed.seconds += magnitude;
        } else if (part->unit == "m") {
            parsed.seconds += magnitude * 60;
        } else if (part->unit == "h") {
            parsed.seconds += magnitude * 60 * 60;
        } else if (part->unit == "d") {
            parsed.seconds += magnitude * 24 * 60 * 60;
        } else if (part->unit == "w") {
            parsed.seconds += magnitude * 7 * 24 * 60 * 60;
        } else if (part->unit == "mo") {
            parsed.months += magnitude;
        } else if (part->unit == "y") {
            parsed.months += magnitude * 12;
        } else {
            return absl::InvalidArgumentError(
                absl::StrCat(name, " unsupported `", property, "` duration unit: ", part->unit));
        }
    }
    return parsed;
}

absl::CivilSecond add_months_to_civil_second(const absl::CivilSecond& civil, int64_t months) {
    int64_t year = civil.year();
    int64_t month0 = static_cast<int64_t>(civil.month()) - 1 + months;
    year += month0 / 12;
    month0 %= 12;
    if (month0 < 0) {
        month0 += 12;
        --year;
    }
    const auto month = static_cast<unsigned>(month0 + 1);
    static constexpr unsigned days_by_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    unsigned max_day = days_by_month[month - 1];
    const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    if (month == 2 && leap) {
        max_day = 29;
    }
    const auto day = std::min(static_cast<unsigned>(civil.day()), max_day);
    return absl::CivilSecond(year, month, day, civil.hour(), civil.minute(), civil.second());
}

int64_t seconds_from_civil_second(const absl::CivilSecond& civil) {
    return utc_seconds_from_civil(
        static_cast<int>(civil.year()), static_cast<unsigned>(civil.month()),
        static_cast<unsigned>(civil.day()), static_cast<unsigned>(civil.hour()),
        static_cast<unsigned>(civil.minute()), static_cast<unsigned>(civil.second()));
}

absl::StatusOr<Value> date_shift(const ObjectValue& object,
                                 const std::string& name,
                                 const std::string& time_property,
                                 int64_t direction) {
    auto duration_or = duration_literal_property(object, name, "d");
    if (!duration_or.ok()) {
        return duration_or.status();
    }
    auto time_or = time_literal_property(object, name, time_property);
    if (!time_or.ok()) {
        return time_or.status();
    }
    auto seconds_or = parse_rfc3339_seconds(*time_or);
    if (!seconds_or.has_value()) {
        return absl::InvalidArgumentError(absl::StrCat(name, " failed to parse RFC3339 time"));
    }
    auto duration = parse_duration_literal(*duration_or, name, "d");
    if (!duration.ok()) {
        return duration.status();
    }

    int64_t shifted = *seconds_or;
    if (duration->months != 0) {
        auto civil = absl::ToCivilSecond(absl::FromUnixSeconds(shifted), absl::UTCTimeZone());
        civil = add_months_to_civil_second(civil, direction * duration->months);
        shifted = seconds_from_civil_second(civil);
    }
    shifted += direction * duration->seconds;
    return Value::time(format_rfc3339_seconds(shifted));
}

int64_t floor_div(int64_t lhs, int64_t rhs) {
    int64_t quotient = lhs / rhs;
    const int64_t remainder = lhs % rhs;
    if (remainder != 0 && ((remainder < 0) != (rhs < 0))) {
        --quotient;
    }
    return quotient;
}

absl::StatusOr<Value> builtin_date_add(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "date.add");
    if (!object_or.ok()) {
        return object_or.status();
    }
    return date_shift(**object_or, "date.add", "to", 1);
}

absl::StatusOr<Value> builtin_date_sub(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "date.sub");
    if (!object_or.ok()) {
        return object_or.status();
    }
    return date_shift(**object_or, "date.sub", "from", -1);
}

absl::StatusOr<Value> builtin_date_truncate(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "date.truncate");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto time_or = time_literal_property(**object_or, "date.truncate", "t");
    if (!time_or.ok()) {
        return time_or.status();
    }
    auto seconds_or = parse_rfc3339_seconds(*time_or);
    if (!seconds_or.has_value()) {
        return absl::InvalidArgumentError("date.truncate failed to parse RFC3339 time");
    }
    auto unit_or = duration_literal_property(**object_or, "date.truncate", "unit");
    if (!unit_or.ok()) {
        return unit_or.status();
    }
    auto duration_or = parse_duration_literal(*unit_or, "date.truncate", "unit", false);
    if (!duration_or.ok()) {
        return duration_or.status();
    }
    if (duration_or->months != 0 && duration_or->seconds != 0) {
        return absl::InvalidArgumentError(
            "date.truncate `unit` cannot mix calendar and fixed units");
    }
    if (duration_or->months == 0 && duration_or->seconds <= 0) {
        return absl::InvalidArgumentError("date.truncate `unit` must be a positive duration");
    }

    int64_t truncated = *seconds_or;
    if (duration_or->months != 0) {
        if (duration_or->months != 1 && duration_or->months != 12) {
            return absl::InvalidArgumentError("date.truncate supports calendar units 1mo or 1y");
        }
        auto civil = absl::ToCivilSecond(absl::FromUnixSeconds(*seconds_or), absl::UTCTimeZone());
        const unsigned month = duration_or->months == 12 ? 1 : static_cast<unsigned>(civil.month());
        truncated = utc_seconds_from_civil(static_cast<int>(civil.year()), month, 1);
    } else {
        truncated = floor_div(*seconds_or, duration_or->seconds) * duration_or->seconds;
    }
    return Value::time(format_rfc3339_seconds(truncated));
}

absl::StatusOr<absl::CivilSecond> date_time_argument(const std::vector<Value>& args,
                                                     const std::string& name) {
    if (args.size() != 1) {
        return absl::InvalidArgumentError(absl::StrCat(name, " expects exactly one argument"));
    }

    const Value* value = &args[0];
    if (args[0].type() == Value::Type::Object) {
        auto value_or = require_object_property(args[0].as_object(), name, "t");
        if (!value_or.ok()) {
            return value_or.status();
        }
        value = *value_or;
    }

    std::string literal;
    if (value->type() == Value::Type::Time) {
        literal = value->as_time().literal;
    } else if (value->type() == Value::Type::String) {
        literal = value->as_string();
    } else {
        return absl::InvalidArgumentError(absl::StrCat(name, " `t` must be a time or string"));
    }

    auto seconds_or = parse_rfc3339_seconds(literal);
    if (!seconds_or.has_value()) {
        return absl::InvalidArgumentError(absl::StrCat(name, " failed to parse RFC3339 time"));
    }
    return absl::ToCivilSecond(absl::FromUnixSeconds(*seconds_or), absl::UTCTimeZone());
}

absl::StatusOr<Value> builtin_date_component(const std::vector<Value>& args,
                                             const std::string& name,
                                             const std::function<int64_t(absl::CivilSecond)>& fn) {
    auto civil_or = date_time_argument(args, name);
    if (!civil_or.ok()) {
        return civil_or.status();
    }
    return Value::integer(fn(*civil_or));
}

absl::StatusOr<Value> builtin_regexp_compile(const std::vector<Value>& args) {
    auto pattern_or = optional_string_value(args, "regexp.compile");
    if (!pattern_or.ok()) {
        return pattern_or.status();
    }
    try {
        (void)std::regex(*pattern_or);
    } catch (const std::regex_error& err) {
        return absl::InvalidArgumentError(
            absl::StrCat("regexp.compile invalid regex: ", err.what()));
    }
    return Value::regex(*pattern_or);
}

absl::StatusOr<Value> builtin_regexp_match_regexp_string(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "regexp.matchRegexpString");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto regexp_or = require_object_property(**object_or, "regexp.matchRegexpString", "r");
    if (!regexp_or.ok()) {
        return regexp_or.status();
    }
    auto value_or = string_property(**object_or, "regexp.matchRegexpString", "v");
    if (!value_or.ok()) {
        return value_or.status();
    }
    std::string pattern;
    if ((*regexp_or)->type() == Value::Type::Regex) {
        pattern = (*regexp_or)->as_regex().literal;
    } else if ((*regexp_or)->type() == Value::Type::String) {
        pattern = (*regexp_or)->as_string();
    } else {
        return absl::InvalidArgumentError("regexp.matchRegexpString `r` must be a regex or string");
    }
    if (pattern.size() >= 2 && pattern.front() == '/' && pattern.back() == '/') {
        pattern = pattern.substr(1, pattern.size() - 2);
    }
    try {
        return Value::boolean(std::regex_search(*value_or, std::regex(pattern)));
    } catch (const std::regex_error& err) {
        return absl::InvalidArgumentError(
            absl::StrCat("regexp.matchRegexpString invalid regex: ", err.what()));
    }
}

absl::StatusOr<Value> builtin_regexp_quote_meta(const std::vector<Value>& args) {
    auto value_or = optional_string_value(args, "regexp.quoteMeta");
    if (!value_or.ok()) {
        return value_or.status();
    }
    std::string quoted;
    for (char ch : *value_or) {
        switch (ch) {
            case '\\':
            case '.':
            case '+':
            case '*':
            case '?':
            case '(':
            case ')':
            case '|':
            case '[':
            case ']':
            case '{':
            case '}':
            case '^':
            case '$':
                quoted.push_back('\\');
                break;
            default:
                break;
        }
        quoted.push_back(ch);
    }
    return Value::string(std::move(quoted));
}

absl::StatusOr<Value> builtin_regexp_find_string(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "regexp.findString");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto regexp_or = require_object_property(**object_or, "regexp.findString", "r");
    if (!regexp_or.ok()) {
        return regexp_or.status();
    }
    auto value_or = string_property(**object_or, "regexp.findString", "v");
    if (!value_or.ok()) {
        return value_or.status();
    }
    std::string pattern;
    if ((*regexp_or)->type() == Value::Type::Regex) {
        pattern = (*regexp_or)->as_regex().literal;
    } else if ((*regexp_or)->type() == Value::Type::String) {
        pattern = (*regexp_or)->as_string();
    } else {
        return absl::InvalidArgumentError("regexp.findString `r` must be a regex or string");
    }
    if (pattern.size() >= 2 && pattern.front() == '/' && pattern.back() == '/') {
        pattern = pattern.substr(1, pattern.size() - 2);
    }
    try {
        std::smatch match;
        if (std::regex_search(*value_or, match, std::regex(pattern))) {
            return Value::string(match.str());
        }
        return Value::string("");
    } catch (const std::regex_error& err) {
        return absl::InvalidArgumentError(
            absl::StrCat("regexp.findString invalid regex: ", err.what()));
    }
}

absl::StatusOr<Value> builtin_strings_contains_str(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "strings.containsStr");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto value_or = string_property(**object_or, "strings.containsStr", "v");
    if (!value_or.ok()) {
        return value_or.status();
    }
    auto substr_or = string_property(**object_or, "strings.containsStr", "substr");
    if (!substr_or.ok()) {
        return substr_or.status();
    }
    return Value::boolean(value_or->find(*substr_or) != std::string::npos);
}

absl::StatusOr<Value> builtin_strings_has_prefix(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "strings.hasPrefix");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto value_or = string_property(**object_or, "strings.hasPrefix", "v");
    if (!value_or.ok()) {
        return value_or.status();
    }
    auto prefix_or = string_property(**object_or, "strings.hasPrefix", "prefix");
    if (!prefix_or.ok()) {
        return prefix_or.status();
    }
    return Value::boolean(value_or->starts_with(*prefix_or));
}

absl::StatusOr<Value> builtin_strings_has_suffix(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "strings.hasSuffix");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto value_or = string_property(**object_or, "strings.hasSuffix", "v");
    if (!value_or.ok()) {
        return value_or.status();
    }
    auto suffix_or = string_property(**object_or, "strings.hasSuffix", "suffix");
    if (!suffix_or.ok()) {
        return suffix_or.status();
    }
    return Value::boolean(value_or->ends_with(*suffix_or));
}

absl::StatusOr<Value> builtin_strings_replace_all(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "strings.replaceAll");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto value_or = string_property(**object_or, "strings.replaceAll", "v");
    if (!value_or.ok()) {
        return value_or.status();
    }
    auto target_or = string_property(**object_or, "strings.replaceAll", "t");
    if (!target_or.ok()) {
        return target_or.status();
    }
    auto replacement_or = string_property(**object_or, "strings.replaceAll", "u");
    if (!replacement_or.ok()) {
        return replacement_or.status();
    }

    std::string out = *value_or;
    if (target_or->empty()) {
        return Value::string(std::move(out));
    }
    size_t pos = 0;
    while ((pos = out.find(*target_or, pos)) != std::string::npos) {
        out.replace(pos, target_or->size(), *replacement_or);
        pos += replacement_or->size();
    }
    return Value::string(std::move(out));
}

absl::StatusOr<Value> builtin_strings_to_upper(const std::vector<Value>& args) {
    auto value_or = optional_string_value(args, "strings.toUpper");
    if (!value_or.ok()) {
        return value_or.status();
    }
    std::string out = *value_or;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return Value::string(std::move(out));
}

absl::StatusOr<Value> builtin_strings_to_lower(const std::vector<Value>& args) {
    auto value_or = optional_string_value(args, "strings.toLower");
    if (!value_or.ok()) {
        return value_or.status();
    }
    std::string out = *value_or;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return Value::string(std::move(out));
}

absl::StatusOr<Value> builtin_strings_split(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "strings.split");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto value_or = string_property(**object_or, "strings.split", "v");
    if (!value_or.ok()) {
        return value_or.status();
    }
    auto sep_or = string_property(**object_or, "strings.split", "t");
    if (!sep_or.ok()) {
        return sep_or.status();
    }
    std::vector<Value> parts;
    if (sep_or->empty()) {
        parts.reserve(value_or->size());
        for (char ch : *value_or) {
            parts.push_back(Value::string(std::string(1, ch)));
        }
        return Value::array(std::move(parts));
    }

    size_t start = 0;
    while (true) {
        const size_t pos = value_or->find(*sep_or, start);
        if (pos == std::string::npos) {
            parts.push_back(Value::string(value_or->substr(start)));
            break;
        }
        parts.push_back(Value::string(value_or->substr(start, pos - start)));
        start = pos + sep_or->size();
    }
    return Value::array(std::move(parts));
}

absl::StatusOr<Value> builtin_strings_join_str(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "strings.joinStr");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto arr_or = require_object_property(**object_or, "strings.joinStr", "arr");
    if (!arr_or.ok()) {
        return arr_or.status();
    }
    if ((*arr_or)->type() != Value::Type::Array) {
        return absl::InvalidArgumentError("strings.joinStr `arr` must be an array");
    }
    auto sep_or = string_property(**object_or, "strings.joinStr", "v");
    if (!sep_or.ok()) {
        return sep_or.status();
    }
    std::string joined;
    bool first = true;
    for (const auto& item : (*arr_or)->as_array().elements) {
        if (item.type() != Value::Type::String) {
            return absl::InvalidArgumentError("strings.joinStr `arr` must contain only strings");
        }
        if (!first) {
            joined += *sep_or;
        }
        first = false;
        joined += item.as_string();
    }
    return Value::string(std::move(joined));
}

absl::StatusOr<Value> builtin_strings_trim_space(const std::vector<Value>& args) {
    auto value_or = optional_string_value(args, "strings.trimSpace");
    if (!value_or.ok()) {
        return value_or.status();
    }
    const auto& value = *value_or;
    size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) {
        ++first;
    }
    size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) {
        --last;
    }
    return Value::string(value.substr(first, last - first));
}

absl::StatusOr<Value> builtin_math_abs(const std::vector<Value>& args) {
    if (args.size() != 1) {
        return absl::InvalidArgumentError("math.abs expects exactly one argument");
    }
    if (args[0].type() == Value::Type::Int) {
        return Value::integer(std::llabs(args[0].as_int()));
    }
    auto number_or = number_argument(args, "math.abs");
    if (!number_or.ok()) {
        return number_or.status();
    }
    return Value::floating(std::fabs(*number_or));
}

absl::StatusOr<Value> builtin_math_unary(const std::vector<Value>& args,
                                         const std::string& name,
                                         double (*fn)(double)) {
    auto number_or = number_argument(args, name);
    if (!number_or.ok()) {
        return number_or.status();
    }
    return Value::floating(fn(*number_or));
}

absl::StatusOr<Value> builtin_math_pow(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "math.pow");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto x_or = number_property(**object_or, "math.pow", "x");
    if (!x_or.ok()) {
        return x_or.status();
    }
    auto y_or = number_property(**object_or, "math.pow", "y");
    if (!y_or.ok()) {
        return y_or.status();
    }
    return Value::floating(std::pow(*x_or, *y_or));
}

absl::StatusOr<Value> builtin_dict_from_list(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "dict.fromList");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto pairs_or = require_object_property(**object_or, "dict.fromList", "pairs");
    if (!pairs_or.ok()) {
        return pairs_or.status();
    }
    if ((*pairs_or)->type() != Value::Type::Array) {
        return absl::InvalidArgumentError("dict.fromList `pairs` must be an array");
    }

    std::vector<std::pair<std::string, Value>> props;
    for (const auto& pair : (*pairs_or)->as_array().elements) {
        if (pair.type() != Value::Type::Object) {
            return absl::InvalidArgumentError("dict.fromList `pairs` must contain records");
        }
        auto key_or = require_object_property(pair.as_object(), "dict.fromList", "key");
        if (!key_or.ok()) {
            return key_or.status();
        }
        auto value_or = require_object_property(pair.as_object(), "dict.fromList", "value");
        if (!value_or.ok()) {
            return value_or.status();
        }
        auto name_or = dict_key_string(**key_or, "dict.fromList");
        if (!name_or.ok()) {
            return name_or.status();
        }

        bool replaced = false;
        for (auto& [key, current] : props) {
            if (key == *name_or) {
                current = **value_or;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            props.emplace_back(*name_or, **value_or);
        }
    }
    return Value::object(std::move(props));
}

absl::StatusOr<Value> builtin_dict_get(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "dict.get");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto dict_or = dict_property(**object_or, "dict.get");
    if (!dict_or.ok()) {
        return dict_or.status();
    }
    auto key_or = require_object_property(**object_or, "dict.get", "key");
    if (!key_or.ok()) {
        return key_or.status();
    }
    auto default_or = require_object_property(**object_or, "dict.get", "default");
    if (!default_or.ok()) {
        return default_or.status();
    }
    auto name_or = dict_key_string(**key_or, "dict.get");
    if (!name_or.ok()) {
        return name_or.status();
    }
    const Value* found = (*dict_or)->lookup(*name_or);
    return found == nullptr ? **default_or : *found;
}

absl::StatusOr<Value> builtin_dict_insert(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "dict.insert");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto dict_or = dict_property(**object_or, "dict.insert");
    if (!dict_or.ok()) {
        return dict_or.status();
    }
    auto key_or = require_object_property(**object_or, "dict.insert", "key");
    if (!key_or.ok()) {
        return key_or.status();
    }
    auto value_or = require_object_property(**object_or, "dict.insert", "value");
    if (!value_or.ok()) {
        return value_or.status();
    }
    auto name_or = dict_key_string(**key_or, "dict.insert");
    if (!name_or.ok()) {
        return name_or.status();
    }

    std::vector<std::pair<std::string, Value>> props = (*dict_or)->properties;
    bool replaced = false;
    for (auto& [key, current] : props) {
        if (key == *name_or) {
            current = **value_or;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        props.emplace_back(*name_or, **value_or);
    }
    return Value::object(std::move(props));
}

absl::StatusOr<Value> builtin_dict_remove(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "dict.remove");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto dict_or = dict_property(**object_or, "dict.remove");
    if (!dict_or.ok()) {
        return dict_or.status();
    }
    auto key_or = require_object_property(**object_or, "dict.remove", "key");
    if (!key_or.ok()) {
        return key_or.status();
    }
    auto name_or = dict_key_string(**key_or, "dict.remove");
    if (!name_or.ok()) {
        return name_or.status();
    }

    std::vector<std::pair<std::string, Value>> props;
    props.reserve((*dict_or)->properties.size());
    for (const auto& [key, value] : (*dict_or)->properties) {
        if (key != *name_or) {
            props.emplace_back(key, value);
        }
    }
    return Value::object(std::move(props));
}

absl::StatusOr<Value> builtin_types_is_numeric(const std::vector<Value>& args) {
    if (args.size() != 1) {
        return absl::InvalidArgumentError("types.isNumeric expects exactly one argument");
    }
    const Value* value = &args[0];
    if (args[0].type() == Value::Type::Object) {
        auto value_or = require_object_property(args[0].as_object(), "types.isNumeric", "v");
        if (!value_or.ok()) {
            return value_or.status();
        }
        value = *value_or;
    }
    return Value::boolean(value->type() == Value::Type::Int || value->type() == Value::Type::UInt ||
                          value->type() == Value::Type::Float);
}

absl::StatusOr<Value> builtin_types_is_type(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "types.isType");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto value_or = require_object_property(**object_or, "types.isType", "v");
    if (!value_or.ok()) {
        return value_or.status();
    }
    auto type_or = string_property(**object_or, "types.isType", "type");
    if (!type_or.ok()) {
        return type_or.status();
    }
    if (*type_or == "bytes") {
        return Value::boolean(false);
    }
    return Value::boolean(value_type_name(**value_or) == *type_or);
}

absl::StatusOr<Value> builtin_types_is_value_type(const std::vector<Value>& args,
                                                  const std::string& name,
                                                  Value::Type type) {
    if (args.size() != 1) {
        return absl::InvalidArgumentError(absl::StrCat(name, " expects exactly one argument"));
    }
    const Value* value = &args[0];
    if (args[0].type() == Value::Type::Object) {
        auto value_or = require_object_property(args[0].as_object(), name, "v");
        if (!value_or.ok()) {
            return value_or.status();
        }
        value = *value_or;
    }
    return Value::boolean(value->type() == type);
}

using JsonBuilder = simdjson::builder::string_builder;

void append_json_field_name(JsonBuilder& builder, std::string_view name, bool& first_field) {
    if (!first_field) {
        builder.append_comma();
    }
    builder.escape_and_append_with_quotes(name);
    builder.append_colon();
    first_field = false;
}

absl::Status append_json_value(JsonBuilder& builder, const Value& value) {
    switch (value.type()) {
        case Value::Type::Null:
            builder.append_null();
            return absl::OkStatus();
        case Value::Type::Bool:
            builder.append(value.as_bool());
            return absl::OkStatus();
        case Value::Type::Int:
            builder.append(value.as_int());
            return absl::OkStatus();
        case Value::Type::UInt:
            builder.append(value.as_uint());
            return absl::OkStatus();
        case Value::Type::Float:
            builder.append(value.as_float());
            return absl::OkStatus();
        case Value::Type::String:
            builder.append(value.as_string());
            return absl::OkStatus();
        case Value::Type::Time:
            builder.append(value.as_time().literal);
            return absl::OkStatus();
        case Value::Type::Duration:
            builder.append(value.as_duration().literal);
            return absl::OkStatus();
        case Value::Type::Regex:
            builder.append(value.as_regex().literal);
            return absl::OkStatus();
        case Value::Type::Array: {
            builder.start_array();
            bool first = true;
            for (const auto& element : value.as_array().elements) {
                if (!first) {
                    builder.append_comma();
                }
                first = false;
                auto status = append_json_value(builder, element);
                if (!status.ok()) {
                    return status;
                }
            }
            builder.end_array();
            return absl::OkStatus();
        }
        case Value::Type::Object: {
            builder.start_object();
            bool first = true;
            for (const auto& [key, property] : value.as_object().properties) {
                append_json_field_name(builder, key, first);
                auto status = append_json_value(builder, property);
                if (!status.ok()) {
                    return status;
                }
            }
            builder.end_object();
            return absl::OkStatus();
        }
        case Value::Type::Table:
            return absl::InvalidArgumentError("json.encode does not support table values");
        case Value::Type::Function:
            return absl::InvalidArgumentError("json.encode does not support function values");
        default:
            PL_FLUX_UNREACHABLE();
    }
}

absl::StatusOr<Value> builtin_json_encode(const std::vector<Value>& args) {
    if (args.size() != 1) {
        return absl::InvalidArgumentError("json.encode expects exactly one argument");
    }
    const Value* value = &args[0];
    if (args[0].type() == Value::Type::Object) {
        if (const Value* property = args[0].as_object().lookup("v"); property != nullptr) {
            value = property;
        }
    }
    JsonBuilder builder;
    auto status = append_json_value(builder, *value);
    if (!status.ok()) {
        return status;
    }
    std::string_view encoded;
    const auto error = builder.view().get(encoded);
    if (error != simdjson::SUCCESS) {
        return absl::InternalError(std::string("failed to serialize json.encode value: ") +
                                   simdjson::error_message(error));
    }
    return Value::string(std::string(encoded));
}

absl::StatusOr<Value> builtin_runtime_version(const std::vector<Value>& args) {
    if (!args.empty()) {
        return absl::InvalidArgumentError("runtime.version expects no arguments");
    }
    return Value::string("playground-flux");
}

absl::StatusOr<Value> builtin_system_time(const std::vector<Value>& args) {
    if (!args.empty()) {
        return absl::InvalidArgumentError("system.time expects no arguments");
    }
    return Value::time(absl::FormatTime("%Y-%m-%dT%H:%M:%SZ", absl::Now(), absl::UTCTimeZone()));
}

Value make_regexp_package() {
    return Value::object({
        {"path", Value::string("regexp")},
        {"compile", make_builtin_value("regexp.compile", builtin_regexp_compile)},
        {"findString", make_builtin_value("regexp.findString", builtin_regexp_find_string, "v")},
        {"matchRegexpString",
         make_builtin_value("regexp.matchRegexpString", builtin_regexp_match_regexp_string, "v")},
        {"quoteMeta", make_builtin_value("regexp.quoteMeta", builtin_regexp_quote_meta)},
    });
}

Value make_strings_package() {
    return Value::object({
        {"path", Value::string("strings")},
        {"containsStr",
         make_builtin_value("strings.containsStr", builtin_strings_contains_str, "v")},
        {"hasPrefix", make_builtin_value("strings.hasPrefix", builtin_strings_has_prefix, "v")},
        {"hasSuffix", make_builtin_value("strings.hasSuffix", builtin_strings_has_suffix, "v")},
        {"joinStr", make_builtin_value("strings.joinStr", builtin_strings_join_str, "arr")},
        {"replaceAll", make_builtin_value("strings.replaceAll", builtin_strings_replace_all, "v")},
        {"split", make_builtin_value("strings.split", builtin_strings_split, "v")},
        {"toUpper", make_builtin_value("strings.toUpper", builtin_strings_to_upper, "v")},
        {"toLower", make_builtin_value("strings.toLower", builtin_strings_to_lower, "v")},
        {"trimSpace", make_builtin_value("strings.trimSpace", builtin_strings_trim_space, "v")},
    });
}

Value make_math_package() {
    return Value::object({
        {"path", Value::string("math")},
        {"pi", Value::floating(3.14159265358979323846)},
        {"abs", make_builtin_value("math.abs", builtin_math_abs, "x")},
        {"ceil", make_builtin_value(
                     "math.ceil",
                     [](const std::vector<Value>& args) {
                         return builtin_math_unary(args, "math.ceil", std::ceil);
                     },
                     "x")},
        {"floor", make_builtin_value(
                      "math.floor",
                      [](const std::vector<Value>& args) {
                          return builtin_math_unary(args, "math.floor", std::floor);
                      },
                      "x")},
        {"round", make_builtin_value(
                      "math.round",
                      [](const std::vector<Value>& args) {
                          return builtin_math_unary(args, "math.round", std::round);
                      },
                      "x")},
        {"sqrt", make_builtin_value(
                     "math.sqrt",
                     [](const std::vector<Value>& args) {
                         return builtin_math_unary(args, "math.sqrt", std::sqrt);
                     },
                     "x")},
        {"pow", make_builtin_value("math.pow", builtin_math_pow)},
    });
}

Value make_date_package() {
    return Value::object({
        {"path", Value::string("date")},
        {"add", make_builtin_value("date.add", builtin_date_add, "to")},
        {"sub", make_builtin_value("date.sub", builtin_date_sub, "from")},
        {"truncate", make_builtin_value("date.truncate", builtin_date_truncate, "t")},
        {"hour", make_builtin_value(
                     "date.hour",
                     [](const std::vector<Value>& args) {
                         return builtin_date_component(
                             args, "date.hour", [](absl::CivilSecond civil) {
                                 return static_cast<int64_t>(civil.hour());
                             });
                     },
                     "t")},
        {"minute", make_builtin_value(
                       "date.minute",
                       [](const std::vector<Value>& args) {
                           return builtin_date_component(
                               args, "date.minute", [](absl::CivilSecond civil) {
                                   return static_cast<int64_t>(civil.minute());
                               });
                       },
                       "t")},
        {"second", make_builtin_value(
                       "date.second",
                       [](const std::vector<Value>& args) {
                           return builtin_date_component(
                               args, "date.second", [](absl::CivilSecond civil) {
                                   return static_cast<int64_t>(civil.second());
                               });
                       },
                       "t")},
        {"monthDay", make_builtin_value(
                         "date.monthDay",
                         [](const std::vector<Value>& args) {
                             return builtin_date_component(
                                 args, "date.monthDay", [](absl::CivilSecond civil) {
                                     return static_cast<int64_t>(civil.day());
                                 });
                         },
                         "t")},
        {"month", make_builtin_value(
                      "date.month",
                      [](const std::vector<Value>& args) {
                          return builtin_date_component(
                              args, "date.month", [](absl::CivilSecond civil) {
                                  return static_cast<int64_t>(civil.month());
                              });
                      },
                      "t")},
        {"year", make_builtin_value(
                     "date.year",
                     [](const std::vector<Value>& args) {
                         return builtin_date_component(
                             args, "date.year", [](absl::CivilSecond civil) {
                                 return static_cast<int64_t>(civil.year());
                             });
                     },
                     "t")},
        {"weekDay", make_builtin_value(
                        "date.weekDay",
                        [](const std::vector<Value>& args) {
                            return builtin_date_component(args, "date.weekDay",
                                                          [](absl::CivilSecond civil) {
                                                              return weekday_sunday_zero(civil);
                                                          });
                        },
                        "t")},
    });
}

Value make_dict_package() {
    return Value::object({
        {"path", Value::string("dict")},
        {"fromList", make_builtin_value("dict.fromList", builtin_dict_from_list)},
        {"get", make_builtin_value("dict.get", builtin_dict_get)},
        {"insert", make_builtin_value("dict.insert", builtin_dict_insert)},
        {"remove", make_builtin_value("dict.remove", builtin_dict_remove)},
    });
}

Value make_json_package() {
    return Value::object({
        {"path", Value::string("json")},
        {"encode", make_builtin_value("json.encode", builtin_json_encode, "v")},
    });
}

Value make_runtime_package() {
    return Value::object({
        {"path", Value::string("runtime")},
        {"version", make_builtin_value("runtime.version", builtin_runtime_version)},
    });
}

Value make_system_package() {
    return Value::object({
        {"path", Value::string("system")},
        {"time", make_builtin_value("system.time", builtin_system_time)},
    });
}

Value make_types_package() {
    return Value::object({
        {"path", Value::string("types")},
        {"isBool", make_builtin_value(
                       "types.isBool",
                       [](const std::vector<Value>& args) {
                           return builtin_types_is_value_type(args, "types.isBool",
                                                              Value::Type::Bool);
                       },
                       "v")},
        {"isDuration", make_builtin_value(
                           "types.isDuration",
                           [](const std::vector<Value>& args) {
                               return builtin_types_is_value_type(args, "types.isDuration",
                                                                  Value::Type::Duration);
                           },
                           "v")},
        {"isFloat", make_builtin_value(
                        "types.isFloat",
                        [](const std::vector<Value>& args) {
                            return builtin_types_is_value_type(args, "types.isFloat",
                                                               Value::Type::Float);
                        },
                        "v")},
        {"isInt", make_builtin_value(
                      "types.isInt",
                      [](const std::vector<Value>& args) {
                          return builtin_types_is_value_type(args, "types.isInt", Value::Type::Int);
                      },
                      "v")},
        {"isNumeric", make_builtin_value("types.isNumeric", builtin_types_is_numeric, "v")},
        {"isRegexp", make_builtin_value(
                         "types.isRegexp",
                         [](const std::vector<Value>& args) {
                             return builtin_types_is_value_type(args, "types.isRegexp",
                                                                Value::Type::Regex);
                         },
                         "v")},
        {"isString", make_builtin_value(
                         "types.isString",
                         [](const std::vector<Value>& args) {
                             return builtin_types_is_value_type(args, "types.isString",
                                                                Value::Type::String);
                         },
                         "v")},
        {"isTime", make_builtin_value(
                       "types.isTime",
                       [](const std::vector<Value>& args) {
                           return builtin_types_is_value_type(args, "types.isTime",
                                                              Value::Type::Time);
                       },
                       "v")},
        {"isType", make_builtin_value("types.isType", builtin_types_is_type, "v")},
        {"isUInt", make_builtin_value(
                       "types.isUInt",
                       [](const std::vector<Value>& args) {
                           return builtin_types_is_value_type(args, "types.isUInt",
                                                              Value::Type::UInt);
                       },
                       "v")},
    });
}

} // namespace

void RegisterScalarStdlibPackages() {
    RegisterPackage("date", make_date_package);
    RegisterPackage("dict", make_dict_package);
    RegisterPackage("regexp", make_regexp_package);
    RegisterPackage("strings", make_strings_package);
    RegisterPackage("math", make_math_package);
    RegisterPackage("json", make_json_package);
    RegisterPackage("runtime", make_runtime_package);
    RegisterPackage("system", make_system_package);
    RegisterPackage("types", make_types_package);
}

} // namespace pl::flux::builtin
