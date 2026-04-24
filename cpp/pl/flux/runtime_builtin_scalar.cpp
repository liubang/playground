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
#include "cpp/pl/flux/runtime_builtin_package.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace pl::flux_builtin {
namespace {

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

std::optional<int64_t> parse_rfc3339_seconds(const std::string& literal) {
    absl::Time timestamp;
    std::string error;
    if (!absl::ParseTime(absl::RFC3339_full, literal, &timestamp, &error)) {
        return std::nullopt;
    }
    return absl::ToUnixSeconds(timestamp);
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

int64_t weekday_sunday_zero(const absl::CivilSecond& civil) {
    const int64_t days =
        days_from_civil(static_cast<int>(civil.year()), static_cast<unsigned>(civil.month()),
                        static_cast<unsigned>(civil.day()));
    return ((days + 4) % 7 + 7) % 7;
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

Value make_regexp_package() {
    return Value::object({
        {"path", Value::string("regexp")},
        {"compile", make_builtin_value("regexp.compile", builtin_regexp_compile)},
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
        {"replaceAll", make_builtin_value("strings.replaceAll", builtin_strings_replace_all, "v")},
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

} // namespace

void RegisterScalarStdlibPackages() {
    RegisterPackage("date", make_date_package);
    RegisterPackage("regexp", make_regexp_package);
    RegisterPackage("strings", make_strings_package);
    RegisterPackage("math", make_math_package);
}

} // namespace pl::flux_builtin
