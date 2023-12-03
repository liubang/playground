// Copyright (c) 2023 The Authors. All rights reserved.
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

#include "ast.h"

#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"

namespace pl {

std::string ArrayItem::string() const { return expression->string(); }

std::string ExprStmt::string() const { return expression->string(); }

std::string ArrayExpr::string() const {
    auto args = absl::StrJoin(elements, ", ", [](std::string* out, const auto& e) {
        out->append(e->string());
    });
    return absl::StrFormat("[ %s ]", args);
}

std::string AttributeParam::string() const { return "value: " + value->string(); }

std::string VariableAssgn::string() const { return id->string() + " = " + init->string(); }

std::string OptionStmt::string() const { return assignment->string(); }

std::string Assignment::string() const {
    std::stringstream ss;
    ss << "Assignment: {";
    switch (type) {
    case Type::VariableAssignment:
        ss << std::get<std::unique_ptr<VariableAssgn>>(value)->string();
        break;
    case Type::MemberAssignment:
        ss << std::get<std::unique_ptr<MemberAssgn>>(value)->string();
        break;
    }
    ss << "}";
    return ss.str();
}

std::string FunctionExpr::string() const {
    std::stringstream ss;
    ss << "(";
    ss << absl::StrJoin(params, ", ", [](std::string* out, const auto& p) {
        out->append(p->string());
    });
    ss << ") -> " << body->string();
    return ss.str();
}

std::string MemberAssgn::string() const {
    std::stringstream ss;
    ss << "member: {";
    ss << member->string();
    ss << "},\n";
    ss << "init: {";
    ss << init->string();
    ss << "}";
    return ss.str();
}

std::string MemberExpr::string() const {
    std::stringstream ss;
    ss << "MemberExpr: {";
    ss << object->string() << "[" << property->string() << "]";
    ss << "}";
    return ss.str();
}

std::string FunctionBody::string() const {
    std::stringstream ss;
    switch (type) {
    case Type::Block:
        ss << std::get<std::unique_ptr<Block>>(body)->string();
        break;
    case Type::Expression:
        ss << std::get<std::unique_ptr<Expression>>(body)->string();
        break;
    }
    return ss.str();
}

std::string Property::string() const {
    std::stringstream ss;
    ss << key->string();
    ss << absl::StrJoin(separator, " ", [](std::string* out, const auto& sep) {
        out->append(sep->text);
    });
    ss << value->string();
    return ss.str();
}

std::string PipeExpr::string() const {
    std::stringstream ss;
    ss << "|> " << call->string() << "(" << argument->string() << ")";
    return ss.str();
}

std::string CallExpr::string() const {
    // auto args = absl::StrJoin(arguments, ", ", [](std::string* out, const auto argument) {
    //     out->append(argument->string());
    // });
    return absl::StrFormat("%s: { %s }", callee->string(), "");
}

std::string PropertyKey::string() const {
    std::stringstream ss;
    switch (type) {
    case Type::Identifier:
        ss << std::get<std::unique_ptr<Identifier>>(key)->string();
        break;
    case Type::StringLiteral:
        ss << std::get<std::unique_ptr<StringLit>>(key)->string();
        break;
    }
    return ss.str();
}
std::string Block::string() const {
    return absl::StrFormat("{%s}", absl::StrJoin(body, ";", [](std::string* out, const auto& b) {
                               out->append(b->string());
                           }));
}

std::string StringLit::string() const { return value; }

std::string FloatLit::string() const { return absl::StrFormat("%.4f", value); }

std::string IntegerLit::string() const { return absl::StrFormat("%d", value); }

std::string Duration::string() const { return absl::StrFormat("%d %s", magnitude, unit); }

std::string DurationLit::string() const {
    return absl::StrJoin(values, ", ", [](std::string* out, const auto& d) {
        out->append(d->string());
    });
}

std::string ReturnStmt::string() const { return absl::StrFormat("return %s", argument->string()); }

std::string LogicalExpr::string() const {
    return absl::StrFormat("%s %s %s", left->string(), op_string(op), right->string());
}

std::string WithSource::string() const {}
std::string ObjectExpr::string() const {
    // todo
    return absl::StrFormat("{}");
}
std::string IndexExpr::string() const {
    // todo
    return absl::StrFormat("[%s]", index->string());
}
std::string BinaryExpr::string() const {
    return absl::StrFormat("%s %s %s", left->string(), op_string(op), right->string());
}
std::string UnaryExpr::string() const {
    return absl::StrFormat("%s %s", op_string(op), argument->string());
}
std::string ConditionalExpr::string() const {}
std::string StringExpr::string() const {}
std::string StringExprPart::string() const {}
std::string TextPart::string() const {}
std::string InterpolatedPart::string() const {}
std::string ParenExpr::string() const {}
std::string BooleanLit::string() const {}
std::string DateTimeLit::string() const {}
std::string RegexpLit::string() const {}
std::string PipeLit::string() const {}
std::string LabelLit::string() const {}
std::string BadExpr::string() const {}
std::string UintLit::string() const {}

std::string Statement::string() const {
    std::stringstream ss;
    switch (type) {
    case Type::ExpressionStatement:
        ss << "ExpressionStatement: ";
        ss << std::get<std::unique_ptr<ExprStmt>>(stmt)->string();
        break;
    case Type::VariableAssignment:
        ss << "VariableAssignment: ";
        ss << std::get<std::unique_ptr<VariableAssgn>>(stmt)->string();
        break;
    case Type::OptionStatement:
        ss << "OptionStatement: ";
        ss << std::get<std::unique_ptr<OptionStmt>>(stmt)->string();
        break;
    case Type::ReturnStatement:
        ss << "ReturnStatement: ";
        ss << std::get<std::unique_ptr<ReturnStmt>>(stmt)->string();
        break;
    case Type::BadStatement:
        ss << "BadStatement: ";
        ss << std::get<std::unique_ptr<BadStmt>>(stmt)->string();
        break;
    case Type::TestCaseStatement:
        ss << "TestCaseStatement: ";
        ss << std::get<std::unique_ptr<TestCaseStmt>>(stmt)->string();
        break;
    case Type::BuiltinStatement:
        ss << "BuiltinStatement: ";
        ss << std::get<std::unique_ptr<BuiltinStmt>>(stmt)->string();
        break;
    }
    return ss.str();
}
std::string Expression::string() const {
    std::stringstream ss;
    ss << "expr: ";
    switch (type) {
    case Type::Identifier:
        ss << std::get<std::unique_ptr<Identifier>>(expr)->string();
        break;
    case Type::ArrayExpr:
        ss << std::get<std::unique_ptr<ArrayExpr>>(expr)->string();
        break;
    case Type::DictExpr:
        ss << std::get<std::unique_ptr<DictExpr>>(expr)->string();
        break;
    case Type::FunctionExpr:
        ss << std::get<std::unique_ptr<FunctionExpr>>(expr)->string();
        break;
    case Type::LogicalExpr:
        ss << std::get<std::unique_ptr<LogicalExpr>>(expr)->string();
        break;
    case Type::ObjectExpr:
        ss << std::get<std::unique_ptr<ObjectExpr>>(expr)->string();
        break;
    case Type::MemberExpr:
        ss << std::get<std::unique_ptr<MemberExpr>>(expr)->string();
        break;
    case Type::IndexExpr:
        ss << std::get<std::unique_ptr<IndexExpr>>(expr)->string();
        break;
    case Type::BinaryExpr:
        ss << std::get<std::unique_ptr<BinaryExpr>>(expr)->string();
        break;
    case Type::UnaryExpr:
        ss << std::get<std::unique_ptr<UnaryExpr>>(expr)->string();
        break;
    case Type::PipeExpr:
        ss << std::get<std::unique_ptr<PipeExpr>>(expr)->string();
        break;
    case Type::CallExpr:
        ss << std::get<std::unique_ptr<CallExpr>>(expr)->string();
        break;
    case Type::ConditionalExpr:
        ss << std::get<std::unique_ptr<ConditionalExpr>>(expr)->string();
        break;
    case Type::StringExpr:
        ss << std::get<std::unique_ptr<StringExpr>>(expr)->string();
        break;
    case Type::ParenExpr:
        ss << std::get<std::unique_ptr<ParenExpr>>(expr)->string();
        break;
    case Type::IntegerLit:
        ss << std::get<std::unique_ptr<IntegerLit>>(expr)->string();
        break;
    case Type::FloatLit:
        ss << std::get<std::unique_ptr<FloatLit>>(expr)->string();
        break;
    case Type::StringLit:
        ss << std::get<std::unique_ptr<StringLit>>(expr)->string();
        break;
    case Type::DurationLit:
        ss << std::get<std::unique_ptr<DurationLit>>(expr)->string();
        break;
    case Type::UnsignedIntegerLit:
        ss << std::get<std::unique_ptr<UintLit>>(expr)->string();
        break;
    case Type::BooleanLit:
        ss << std::get<std::unique_ptr<BooleanLit>>(expr)->string();
        break;
    case Type::DateTimeLit:
        ss << std::get<std::unique_ptr<DateTimeLit>>(expr)->string();
        break;
    case Type::RegexpLit:
        ss << std::get<std::unique_ptr<RegexpLit>>(expr)->string();
        break;
    case Type::PipeLit:
        ss << std::get<std::unique_ptr<PipeLit>>(expr)->string();
        break;
    case Type::LabelLit:
        ss << std::get<std::unique_ptr<LabelLit>>(expr)->string();
        break;
    case Type::BadExpr:
        ss << std::get<std::unique_ptr<BadExpr>>(expr)->string();
        break;
    }
    return ss.str();
}
} // namespace pl
