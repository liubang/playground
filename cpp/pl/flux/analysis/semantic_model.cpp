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
// Created: 2026/05/24 11:38

#include "cpp/pl/flux/analysis/semantic_model.h"

#include <algorithm>
#include <memory>
#include <sstream>

namespace pl::flux::analysis {
namespace {

std::string trim(std::string_view text) {
    auto begin = text.find_first_not_of(" \t\n\r");
    if (begin == std::string_view::npos) {
        return "";
    }
    auto end = text.find_last_not_of(" \t\n\r");
    return std::string(text.substr(begin, end - begin + 1));
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.starts_with(prefix);
}

bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

} // namespace

Type Type::Unknown() {
    return {.kind = TypeKind::Unknown};
}

Type Type::Dynamic() {
    return {.kind = TypeKind::Dynamic};
}

Type Type::Error() {
    return {.kind = TypeKind::Error};
}

Type Type::Scalar(TypeKind kind) {
    return {.kind = kind};
}

Type Type::Array(Type element) {
    Type type;
    type.kind = TypeKind::Array;
    type.args.push_back(std::move(element));
    return type;
}

Type Type::Dict(Type key, Type value) {
    Type type;
    type.kind = TypeKind::Dict;
    type.args.push_back(std::move(key));
    type.args.push_back(std::move(value));
    return type;
}

Type Type::Record(std::vector<RecordFieldType> fields_in, bool open) {
    Type type;
    type.kind = TypeKind::Record;
    type.fields = std::move(fields_in);
    type.open_record = open;
    return type;
}

Type Type::Function(std::vector<FunctionParamType> params_in, Type result) {
    Type type;
    type.kind = TypeKind::Function;
    type.params = std::move(params_in);
    type.args.push_back(std::move(result));
    return type;
}

Type Type::Stream(Type row) {
    Type type;
    type.kind = TypeKind::Stream;
    type.args.push_back(std::move(row));
    return type;
}

Type Type::Table(Type row) {
    Type type;
    type.kind = TypeKind::Table;
    type.args.push_back(std::move(row));
    return type;
}

bool Type::IsUnknownLike() const {
    return kind == TypeKind::Unknown || kind == TypeKind::Dynamic || kind == TypeKind::Error;
}

bool Type::IsNumeric() const {
    return kind == TypeKind::Int || kind == TypeKind::UInt || kind == TypeKind::Float;
}

bool Type::IsBool() const {
    return kind == TypeKind::Bool;
}

bool Type::IsString() const {
    return kind == TypeKind::String || kind == TypeKind::Label;
}

bool Type::IsStreamLike() const {
    return kind == TypeKind::Stream || kind == TypeKind::Table;
}

std::optional<Type> Type::Field(std::string_view name_in) const {
    if (kind != TypeKind::Record) {
        return std::nullopt;
    }
    for (const auto& field : fields) {
        if (field.name == name_in) {
            return field.type ? *field.type : Type::Unknown();
        }
    }
    return std::nullopt;
}

std::string Type::ToString() const {
    switch (kind) {
        case TypeKind::Unknown:
            return "unknown";
        case TypeKind::Dynamic:
            return "dynamic";
        case TypeKind::Null:
            return "null";
        case TypeKind::Bool:
            return "bool";
        case TypeKind::Int:
            return "int";
        case TypeKind::UInt:
            return "uint";
        case TypeKind::Float:
            return "float";
        case TypeKind::String:
            return "string";
        case TypeKind::Time:
            return "time";
        case TypeKind::Duration:
            return "duration";
        case TypeKind::Regexp:
            return "regexp";
        case TypeKind::Label:
            return "label";
        case TypeKind::Array:
            return "[" + (args.empty() ? Type::Unknown().ToString() : args[0].ToString()) + "]";
        case TypeKind::Dict:
            return "[" + (args.empty() ? Type::Unknown().ToString() : args[0].ToString()) + ":" +
                   (args.size() < 2 ? Type::Unknown().ToString() : args[1].ToString()) + "]";
        case TypeKind::Record: {
            std::ostringstream out;
            out << "{";
            if (open_record) {
                out << "...";
                if (!fields.empty()) {
                    out << ", ";
                }
            }
            for (size_t i = 0; i < fields.size(); ++i) {
                if (i != 0) {
                    out << ", ";
                }
                out << fields[i].name << ": "
                    << (fields[i].type ? fields[i].type->ToString() : Type::Unknown().ToString());
            }
            out << "}";
            return out.str();
        }
        case TypeKind::Function: {
            std::ostringstream out;
            out << "(";
            for (size_t i = 0; i < params.size(); ++i) {
                if (i != 0) {
                    out << ", ";
                }
                out << params[i].name << ": "
                    << (params[i].type ? params[i].type->ToString() : Type::Unknown().ToString());
            }
            out << ") => " << (args.empty() ? Type::Unknown().ToString() : args[0].ToString());
            return out.str();
        }
        case TypeKind::Stream:
            return "stream[" + (args.empty() ? Type::Unknown().ToString() : args[0].ToString()) +
                   "]";
        case TypeKind::Table:
            return "table[" + (args.empty() ? Type::Unknown().ToString() : args[0].ToString()) +
                   "]";
        case TypeKind::Error:
            return "error";
    }
    return "unknown";
}

bool SameType(const Type& lhs, const Type& rhs) {
    if (lhs.kind != rhs.kind) {
        return false;
    }
    if (lhs.args.size() != rhs.args.size() || lhs.fields.size() != rhs.fields.size() ||
        lhs.params.size() != rhs.params.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.args.size(); ++i) {
        if (!SameType(lhs.args[i], rhs.args[i])) {
            return false;
        }
    }
    for (size_t i = 0; i < lhs.fields.size(); ++i) {
        if (lhs.fields[i].name != rhs.fields[i].name || !lhs.fields[i].type ||
            !rhs.fields[i].type || !SameType(*lhs.fields[i].type, *rhs.fields[i].type)) {
            return false;
        }
    }
    return true;
}

bool CanAssign(const Type& expected, const Type& actual) {
    if (expected.IsUnknownLike() || actual.IsUnknownLike()) {
        return true;
    }
    if (SameType(expected, actual)) {
        return true;
    }
    if (expected.kind == TypeKind::Float && actual.IsNumeric()) {
        return true;
    }
    if (expected.kind == TypeKind::String && actual.kind == TypeKind::Label) {
        return true;
    }
    if (expected.kind == TypeKind::Function && actual.kind == TypeKind::Function &&
        expected.params.empty()) {
        return true;
    }
    if (expected.kind == TypeKind::Array && actual.kind == TypeKind::Array &&
        !expected.args.empty() && !actual.args.empty()) {
        return CanAssign(expected.args[0], actual.args[0]);
    }
    if ((expected.kind == TypeKind::Stream || expected.kind == TypeKind::Table) &&
        expected.kind == actual.kind && !expected.args.empty() && !actual.args.empty()) {
        return CanAssign(expected.args[0], actual.args[0]);
    }
    if (expected.kind == TypeKind::Record && actual.kind == TypeKind::Record &&
        expected.open_record && expected.fields.empty()) {
        return true;
    }
    if (expected.kind == TypeKind::Dict && actual.kind == TypeKind::Dict &&
        expected.args.size() == 2 && actual.args.size() == 2) {
        return CanAssign(expected.args[0], actual.args[0]) &&
               CanAssign(expected.args[1], actual.args[1]);
    }
    return false;
}

Type CommonType(const Type& lhs, const Type& rhs) {
    if (lhs.kind == TypeKind::Error || rhs.kind == TypeKind::Error) {
        return Type::Error();
    }
    if (lhs.kind == TypeKind::Unknown) {
        return rhs;
    }
    if (rhs.kind == TypeKind::Unknown) {
        return lhs;
    }
    if (lhs.kind == TypeKind::Dynamic || rhs.kind == TypeKind::Dynamic) {
        return Type::Dynamic();
    }
    if (SameType(lhs, rhs)) {
        return lhs;
    }
    if (lhs.IsNumeric() && rhs.IsNumeric()) {
        return lhs.kind == TypeKind::Float || rhs.kind == TypeKind::Float
                   ? Type::Scalar(TypeKind::Float)
                   : Type::Scalar(TypeKind::Int);
    }
    if (lhs.kind == TypeKind::Record && rhs.kind == TypeKind::Record) {
        std::vector<RecordFieldType> fields;
        for (const auto& left_field : lhs.fields) {
            auto right = rhs.Field(left_field.name);
            if (right.has_value()) {
                auto left = left_field.type ? *left_field.type : Type::Unknown();
                fields.push_back({.name = left_field.name,
                                  .type = std::make_shared<Type>(CommonType(left, *right))});
            }
        }
        return Type::Record(std::move(fields), lhs.open_record || rhs.open_record);
    }
    if (lhs.kind == TypeKind::Array && rhs.kind == TypeKind::Array && !lhs.args.empty() &&
        !rhs.args.empty()) {
        return Type::Array(CommonType(lhs.args[0], rhs.args[0]));
    }
    return Type::Dynamic();
}

Type ParseTypeExpression(std::string_view text_in) {
    auto text = trim(text_in);
    if (text.empty() || text == "A" || text == "B" || text == "C") {
        return Type::Dynamic();
    }
    if (text == "bool") {
        return Type::Scalar(TypeKind::Bool);
    }
    if (text == "int") {
        return Type::Scalar(TypeKind::Int);
    }
    if (text == "uint") {
        return Type::Scalar(TypeKind::UInt);
    }
    if (text == "float") {
        return Type::Scalar(TypeKind::Float);
    }
    if (text == "string") {
        return Type::Scalar(TypeKind::String);
    }
    if (text == "time") {
        return Type::Scalar(TypeKind::Time);
    }
    if (text == "duration") {
        return Type::Scalar(TypeKind::Duration);
    }
    if (text == "regexp") {
        return Type::Scalar(TypeKind::Regexp);
    }
    if (text == "record") {
        return Type::Record({}, true);
    }
    if (starts_with(text, "stream[") && ends_with(text, "]")) {
        return Type::Stream(ParseTypeExpression(std::string_view(text).substr(7, text.size() - 8)));
    }
    if (starts_with(text, "[") && ends_with(text, "]")) {
        auto inner = std::string_view(text).substr(1, text.size() - 2);
        auto colon = inner.find(':');
        if (colon != std::string_view::npos) {
            return Type::Dict(ParseTypeExpression(inner.substr(0, colon)),
                              ParseTypeExpression(inner.substr(colon + 1)));
        }
        return Type::Array(ParseTypeExpression(inner));
    }
    if (text.find('|') != std::string::npos) {
        return Type::Dynamic();
    }
    if (text.find("=>") != std::string::npos) {
        return Type::Function({}, Type::Dynamic());
    }
    return Type::Dynamic();
}

} // namespace pl::flux::analysis
