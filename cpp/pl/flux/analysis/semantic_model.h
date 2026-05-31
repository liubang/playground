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

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/pl/flux/syntax/ast.h"

namespace pl::flux::analysis {

enum class TypeKind {
    Unknown,
    Dynamic,
    Null,
    Bool,
    Int,
    UInt,
    Float,
    String,
    Time,
    Duration,
    Regexp,
    Label,
    Array,
    Dict,
    Record,
    Function,
    Stream,
    Table,
    Error,
};

struct Type;

struct RecordFieldType {
    std::string name{};
    std::shared_ptr<Type> type{};
};

struct FunctionParamType {
    std::string name{};
    std::shared_ptr<Type> type{};
    bool optional = false;
    bool pipe = false;
};

struct Type {
    TypeKind kind = TypeKind::Unknown;
    std::string name{};
    std::vector<Type> args{};
    std::vector<RecordFieldType> fields{};
    std::vector<FunctionParamType> params{};
    bool open_record = false;

    static Type Unknown();
    static Type Dynamic();
    static Type Error();
    static Type Scalar(TypeKind kind);
    static Type Array(Type element);
    static Type Dict(Type key, Type value);
    static Type Record(std::vector<RecordFieldType> fields, bool open = false);
    static Type Function(std::vector<FunctionParamType> params, Type result);
    static Type Stream(Type row);
    static Type Table(Type row);

    [[nodiscard]] bool IsUnknownLike() const;
    [[nodiscard]] bool IsNumeric() const;
    [[nodiscard]] bool IsBool() const;
    [[nodiscard]] bool IsString() const;
    [[nodiscard]] bool IsStreamLike() const;
    [[nodiscard]] std::optional<Type> Field(std::string_view name) const;
    [[nodiscard]] std::string ToString() const;
};

bool SameType(const Type& lhs, const Type& rhs);
bool CanAssign(const Type& expected, const Type& actual);
Type CommonType(const Type& lhs, const Type& rhs);
Type ParseTypeExpression(std::string_view text);

struct ExpressionInfo {
    SourceLocation location;
    Type type;
    std::optional<size_t> reference_index;
};

struct CallInfo {
    SourceLocation location;
    SourceLocation callee_location;
    std::string callee;
    std::optional<std::string> package;
    std::optional<std::string> member;
    std::vector<std::string> argument_names;
    Type return_type;
    bool pipe_value_present = false;
    bool builtin = false;
};

struct RecordSchema {
    SourceLocation location;
    std::vector<RecordFieldType> fields;
    bool open = false;
};

struct TableSchema {
    SourceLocation location;
    std::vector<RecordFieldType> columns;
    std::vector<std::string> group_key;
    bool open = false;
};

} // namespace pl::flux::analysis
