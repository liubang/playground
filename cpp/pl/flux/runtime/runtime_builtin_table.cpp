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
// Created: 2026/04/25 09:26

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "cpp/pl/flux/connector/memory_source.h"
#include "cpp/pl/flux/runtime/runtime_builtin_package.h"
#include "cpp/pl/flux/runtime/runtime_eval.h"
#include <exception>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace pl::flux::builtin {
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

absl::StatusOr<const ArrayValue*> require_array_property(const ObjectValue& object,
                                                         const std::string& name,
                                                         const std::string& property) {
    auto value_or = require_object_property(object, name, property);
    if (!value_or.ok()) {
        return value_or.status();
    }
    if ((*value_or)->type() != Value::Type::Array) {
        return absl::InvalidArgumentError(absl::StrCat(name, " `", property, "` must be an array"));
    }
    return &(*value_or)->as_array();
}

absl::StatusOr<const FunctionValue*> require_function_property(const ObjectValue& object,
                                                               const std::string& name,
                                                               const std::string& property) {
    auto value_or = require_object_property(object, name, property);
    if (!value_or.ok()) {
        return value_or.status();
    }
    if ((*value_or)->type() != Value::Type::Function) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " `", property, "` must be a function"));
    }
    return &(*value_or)->as_function();
}

absl::StatusOr<std::string> optional_string_property(const ObjectValue& object,
                                                     const std::string& name,
                                                     const std::string& property,
                                                     std::string default_value) {
    const Value* value = object.lookup(property);
    if (value == nullptr) {
        return default_value;
    }
    if (value->type() != Value::Type::String) {
        return absl::InvalidArgumentError(absl::StrCat(name, " `", property, "` must be a string"));
    }
    return value->as_string();
}

absl::StatusOr<std::vector<std::shared_ptr<ObjectValue>>> require_table_rows(
    const Value& value, const std::string& name) {
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

absl::StatusOr<std::vector<std::string>> parse_csv_record(const std::string& line,
                                                          const std::string& name) {
    if (line.find('"') == std::string::npos) {
        size_t field_count = 1;
        for (char ch : line) {
            if (ch == ',') {
                ++field_count;
            }
        }
        std::vector<std::string> fields;
        fields.reserve(field_count);
        size_t start = 0;
        while (true) {
            const size_t comma = line.find(',', start);
            if (comma == std::string::npos) {
                fields.emplace_back(line.substr(start));
                return fields;
            }
            fields.emplace_back(line.substr(start, comma - start));
            start = comma + 1;
        }
    }

    std::vector<std::string> fields;
    fields.reserve(8);
    std::string field;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (quoted) {
            if (ch == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    field.push_back('"');
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field.push_back(ch);
            }
            continue;
        }
        if (ch == ',') {
            fields.push_back(field);
            field.clear();
        } else if (ch == '"') {
            if (!field.empty()) {
                return absl::InvalidArgumentError(
                    absl::StrCat(name, " CSV quote must start a field"));
            }
            quoted = true;
        } else {
            field.push_back(ch);
        }
    }
    if (quoted) {
        return absl::InvalidArgumentError(absl::StrCat(name, " CSV has an unterminated quote"));
    }
    fields.push_back(field);
    return fields;
}

absl::StatusOr<Value> parse_raw_csv_table(const std::string& csv, const std::string& name) {
    std::istringstream input(csv);
    std::string line;
    std::vector<std::string> header;
    std::vector<std::shared_ptr<ObjectValue>> rows;

    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        auto fields_or = parse_csv_record(line, name);
        if (!fields_or.ok()) {
            return fields_or.status();
        }
        if (header.empty()) {
            header = std::move(*fields_or);
            continue;
        }
        if (fields_or->size() != header.size()) {
            return absl::InvalidArgumentError(absl::StrCat(name, " CSV row has ", fields_or->size(),
                                                           " fields, expected ", header.size()));
        }
        std::vector<std::pair<std::string, Value>> properties;
        properties.reserve(header.size());
        for (size_t i = 0; i < header.size(); ++i) {
            properties.emplace_back(header[i], Value::string((*fields_or)[i]));
        }
        rows.push_back(std::make_shared<ObjectValue>(std::move(properties)));
    }
    if (header.empty()) {
        return absl::InvalidArgumentError(absl::StrCat(name, " CSV must include a header row"));
    }
    connector::CsvSource source(std::move(rows));
    return source.Scan({});
}

bool csv_bool(const std::string& value) {
    return value == "true" || value == "TRUE" || value == "True";
}

absl::StatusOr<Value> parse_annotated_csv_value(const std::string& raw,
                                                const std::string& datatype,
                                                const std::string& name) {
    if (raw.empty()) {
        return Value::null();
    }
    try {
        size_t consumed = 0;
        if (datatype == "string" || datatype == "base64Binary") {
            return Value::string(raw);
        }
        if (datatype == "long") {
            auto value = std::stoll(raw, &consumed, 10);
            if (consumed != raw.size()) {
                return absl::InvalidArgumentError(absl::StrCat(name, " invalid long value: ", raw));
            }
            return Value::integer(value);
        }
        if (datatype == "unsignedLong") {
            auto value = std::stoull(raw, &consumed, 10);
            if (consumed != raw.size()) {
                return absl::InvalidArgumentError(
                    absl::StrCat(name, " invalid unsignedLong value: ", raw));
            }
            return Value::uinteger(value);
        }
        if (datatype == "double") {
            auto value = std::stod(raw, &consumed);
            if (consumed != raw.size()) {
                return absl::InvalidArgumentError(
                    absl::StrCat(name, " invalid double value: ", raw));
            }
            return Value::floating(value);
        }
        if (datatype == "boolean" || datatype == "bool") {
            if (csv_bool(raw)) {
                return Value::boolean(true);
            }
            if (raw == "false" || raw == "FALSE" || raw == "False") {
                return Value::boolean(false);
            }
            return absl::InvalidArgumentError(absl::StrCat(name, " invalid boolean value: ", raw));
        }
        if (datatype == "dateTime:RFC3339" || datatype == "dateTime:RFC3339Nano") {
            return Value::time(raw);
        }
        if (datatype == "duration") {
            return Value::duration(raw);
        }
    } catch (const std::exception&) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " invalid ", datatype, " value: ", raw));
    }
    return Value::string(raw);
}

absl::StatusOr<Value> parse_annotated_csv_table(const std::string& csv, const std::string& name) {
    std::istringstream input(csv);
    std::string line;
    std::vector<std::string> datatypes;
    std::vector<bool> groups;
    std::vector<std::string> defaults;
    std::vector<std::string> header;
    std::vector<std::shared_ptr<ObjectValue>> rows;

    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        auto fields_or = parse_csv_record(line, name);
        if (!fields_or.ok()) {
            return fields_or.status();
        }
        auto fields = std::move(*fields_or);
        if (fields.empty()) {
            continue;
        }
        if (fields[0] == "#datatype") {
            datatypes = std::move(fields);
            groups.clear();
            defaults.clear();
            header.clear();
            continue;
        }
        if (fields[0] == "#group") {
            groups.clear();
            groups.reserve(fields.size());
            for (const auto& field : fields) {
                groups.push_back(csv_bool(field));
            }
            continue;
        }
        if (fields[0] == "#default") {
            defaults = std::move(fields);
            continue;
        }
        if (header.empty()) {
            header = std::move(fields);
            if (datatypes.size() != header.size()) {
                return absl::InvalidArgumentError(
                    absl::StrCat(name, " annotated CSV requires #datatype for each column: got ",
                                 datatypes.size(), ", expected ", header.size()));
            }
            if (groups.size() != header.size()) {
                return absl::InvalidArgumentError(
                    absl::StrCat(name, " annotated CSV requires #group for each column: got ",
                                 groups.size(), ", expected ", header.size()));
            }
            if (defaults.empty()) {
                defaults.resize(header.size());
            }
            if (defaults.size() != header.size()) {
                return absl::InvalidArgumentError(
                    absl::StrCat(name, " annotated CSV #default column count mismatch: got ",
                                 defaults.size(), ", expected ", header.size()));
            }
            continue;
        }
        if (fields.size() != header.size()) {
            return absl::InvalidArgumentError(absl::StrCat(name, " CSV row has ", fields.size(),
                                                           " fields, expected ", header.size()));
        }
        std::vector<std::pair<std::string, Value>> properties;
        std::vector<std::pair<std::string, Value>> group_properties;
        for (size_t i = 0; i < header.size(); ++i) {
            if (header[i].empty()) {
                continue;
            }
            const std::string& raw = fields[i].empty() ? defaults[i] : fields[i];
            auto value_or = parse_annotated_csv_value(raw, datatypes[i], name);
            if (!value_or.ok()) {
                return value_or.status();
            }
            properties.emplace_back(header[i], *value_or);
            if (groups[i]) {
                group_properties.emplace_back(header[i], *value_or);
            }
        }
        if (!group_properties.empty()) {
            properties.emplace_back("_group", Value::object(std::move(group_properties)));
        }
        rows.push_back(std::make_shared<ObjectValue>(std::move(properties)));
    }
    if (header.empty()) {
        return absl::InvalidArgumentError(
            absl::StrCat(name, " annotated CSV must include a header row"));
    }
    connector::CsvSource source(std::move(rows));
    return source.Scan({});
}

absl::StatusOr<std::string> read_text_file(const std::string& path, const std::string& name) {
    std::ifstream file(path);
    if (!file) {
        return absl::NotFoundError(absl::StrCat(name, " cannot open file: ", path));
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

absl::StatusOr<Value> builtin_array_from(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.from");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto rows_or = require_object_property(**object_or, "array.from", "rows");
    if (!rows_or.ok()) {
        return rows_or.status();
    }
    auto parsed_rows_or = require_table_rows(**rows_or, "array.from");
    if (!parsed_rows_or.ok()) {
        return parsed_rows_or.status();
    }
    auto bucket_or = optional_string_property(**object_or, "array.from", "bucket", "array");
    if (!bucket_or.ok()) {
        return bucket_or.status();
    }
    connector::ArraySource source(*bucket_or, std::move(*parsed_rows_or));
    return source.Scan({});
}

absl::StatusOr<Value> builtin_array_concat(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.concat");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto arr_or = require_array_property(**object_or, "array.concat", "arr");
    if (!arr_or.ok()) {
        return arr_or.status();
    }
    auto v_or = require_array_property(**object_or, "array.concat", "v");
    if (!v_or.ok()) {
        return v_or.status();
    }

    std::vector<Value> elements;
    elements.reserve((*arr_or)->elements.size() + (*v_or)->elements.size());
    elements.insert(elements.end(), (*arr_or)->elements.begin(), (*arr_or)->elements.end());
    elements.insert(elements.end(), (*v_or)->elements.begin(), (*v_or)->elements.end());
    return Value::array(std::move(elements));
}

absl::StatusOr<Value> builtin_array_filter(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.filter");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto arr_or = require_array_property(**object_or, "array.filter", "arr");
    if (!arr_or.ok()) {
        return arr_or.status();
    }
    auto fn_or = require_function_property(**object_or, "array.filter", "fn");
    if (!fn_or.ok()) {
        return fn_or.status();
    }

    const Value fn = Value::function(std::make_shared<FunctionValue>(**fn_or));
    std::vector<Value> filtered;
    filtered.reserve((*arr_or)->elements.size());
    for (const auto& element : (*arr_or)->elements) {
        auto keep_or = ExpressionEvaluator::Invoke(fn, {element});
        if (!keep_or.ok()) {
            return keep_or.status();
        }
        if (keep_or->type() != Value::Type::Bool) {
            return absl::InvalidArgumentError("array.filter `fn` must return a boolean");
        }
        if (keep_or->as_bool()) {
            filtered.push_back(element);
        }
    }
    return Value::array(std::move(filtered));
}

absl::StatusOr<Value> builtin_array_map(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.map");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto arr_or = require_array_property(**object_or, "array.map", "arr");
    if (!arr_or.ok()) {
        return arr_or.status();
    }
    auto fn_or = require_function_property(**object_or, "array.map", "fn");
    if (!fn_or.ok()) {
        return fn_or.status();
    }

    const Value fn = Value::function(std::make_shared<FunctionValue>(**fn_or));
    std::vector<Value> mapped;
    mapped.reserve((*arr_or)->elements.size());
    for (const auto& element : (*arr_or)->elements) {
        auto mapped_or = ExpressionEvaluator::Invoke(fn, {element});
        if (!mapped_or.ok()) {
            return mapped_or.status();
        }
        mapped.push_back(*mapped_or);
    }
    return Value::array(std::move(mapped));
}

absl::StatusOr<Value> builtin_array_contains(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.contains");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto arr_or = require_array_property(**object_or, "array.contains", "arr");
    if (!arr_or.ok()) {
        return arr_or.status();
    }
    auto value_or = require_object_property(**object_or, "array.contains", "value");
    if (!value_or.ok()) {
        return value_or.status();
    }

    for (const auto& element : (*arr_or)->elements) {
        if (element == **value_or) {
            return Value::boolean(true);
        }
    }
    return Value::boolean(false);
}

absl::StatusOr<Value> builtin_array_reduce(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.reduce");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto arr_or = require_array_property(**object_or, "array.reduce", "arr");
    if (!arr_or.ok()) {
        return arr_or.status();
    }
    auto fn_or = require_function_property(**object_or, "array.reduce", "fn");
    if (!fn_or.ok()) {
        return fn_or.status();
    }
    auto identity_or = require_object_property(**object_or, "array.reduce", "identity");
    if (!identity_or.ok()) {
        return identity_or.status();
    }

    const Value fn = Value::function(std::make_shared<FunctionValue>(**fn_or));
    Value accumulator = **identity_or;
    for (const auto& element : (*arr_or)->elements) {
        auto next_or = ExpressionEvaluator::Invoke(fn, {element, accumulator});
        if (!next_or.ok()) {
            return next_or.status();
        }
        accumulator = *next_or;
    }
    return accumulator;
}

absl::StatusOr<Value> builtin_array_any(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.any");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto arr_or = require_array_property(**object_or, "array.any", "arr");
    if (!arr_or.ok()) {
        return arr_or.status();
    }
    auto fn_or = require_function_property(**object_or, "array.any", "fn");
    if (!fn_or.ok()) {
        return fn_or.status();
    }

    const Value fn = Value::function(std::make_shared<FunctionValue>(**fn_or));
    for (const auto& element : (*arr_or)->elements) {
        auto matched_or = ExpressionEvaluator::Invoke(fn, {element});
        if (!matched_or.ok()) {
            return matched_or.status();
        }
        if (matched_or->type() != Value::Type::Bool) {
            return absl::InvalidArgumentError("array.any `fn` must return a boolean");
        }
        if (matched_or->as_bool()) {
            return Value::boolean(true);
        }
    }
    return Value::boolean(false);
}

absl::StatusOr<Value> builtin_array_all(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "array.all");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto arr_or = require_array_property(**object_or, "array.all", "arr");
    if (!arr_or.ok()) {
        return arr_or.status();
    }
    auto fn_or = require_function_property(**object_or, "array.all", "fn");
    if (!fn_or.ok()) {
        return fn_or.status();
    }

    const Value fn = Value::function(std::make_shared<FunctionValue>(**fn_or));
    for (const auto& element : (*arr_or)->elements) {
        auto matched_or = ExpressionEvaluator::Invoke(fn, {element});
        if (!matched_or.ok()) {
            return matched_or.status();
        }
        if (matched_or->type() != Value::Type::Bool) {
            return absl::InvalidArgumentError("array.all `fn` must return a boolean");
        }
        if (!matched_or->as_bool()) {
            return Value::boolean(false);
        }
    }
    return Value::boolean(true);
}

absl::StatusOr<Value> builtin_csv_from(const std::vector<Value>& args) {
    auto object_or = require_object_argument(args, "csv.from");
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto mode_or = optional_string_property(**object_or, "csv.from", "mode", "annotations");
    if (!mode_or.ok()) {
        return mode_or.status();
    }
    if (*mode_or != "raw" && *mode_or != "annotations") {
        return absl::InvalidArgumentError(R"(csv.from `mode` must be "raw" or "annotations")");
    }

    const Value* csv_value = (*object_or)->lookup("csv");
    const Value* file_value = (*object_or)->lookup("file");
    if ((csv_value == nullptr) == (file_value == nullptr)) {
        return absl::InvalidArgumentError("csv.from requires exactly one of `csv` or `file`");
    }
    if (csv_value != nullptr) {
        if (csv_value->type() != Value::Type::String) {
            return absl::InvalidArgumentError("csv.from `csv` must be a string");
        }
        if (*mode_or == "raw") {
            return parse_raw_csv_table(csv_value->as_string(), "csv.from");
        }
        return parse_annotated_csv_table(csv_value->as_string(), "csv.from");
    }
    if (file_value->type() != Value::Type::String) {
        return absl::InvalidArgumentError("csv.from `file` must be a string");
    }
    auto csv_or = read_text_file(file_value->as_string(), "csv.from");
    if (!csv_or.ok()) {
        return csv_or.status();
    }
    if (*mode_or == "raw") {
        return parse_raw_csv_table(*csv_or, "csv.from");
    }
    return parse_annotated_csv_table(*csv_or, "csv.from");
}

Value make_array_package() {
    return Value::object({
        {"path", Value::string("array")},
        {"from", make_builtin_value("array.from", builtin_array_from)},
        {"concat", make_builtin_value("array.concat", builtin_array_concat, "arr")},
        {"filter", make_builtin_value("array.filter", builtin_array_filter, "arr")},
        {"map", make_builtin_value("array.map", builtin_array_map, "arr")},
        {"contains", make_builtin_value("array.contains", builtin_array_contains, "arr")},
        {"reduce", make_builtin_value("array.reduce", builtin_array_reduce, "arr")},
        {"any", make_builtin_value("array.any", builtin_array_any, "arr")},
        {"all", make_builtin_value("array.all", builtin_array_all, "arr")},
    });
}

Value make_csv_package() {
    return Value::object({
        {"path", Value::string("csv")},
        {"from", make_builtin_value("csv.from", builtin_csv_from)},
    });
}

} // namespace

void RegisterTableStdlibPackages() {
    RegisterPackage("array", make_array_package);
    RegisterPackage("csv", make_csv_package);
}

} // namespace pl::flux::builtin
