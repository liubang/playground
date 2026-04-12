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

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"

namespace pl {

namespace {

std::string mono_type_string(const MonoType& mono);
std::string attributes_prefix(const std::vector<std::shared_ptr<Attribute>>& attributes) {
    if (attributes.empty()) {
        return "";
    }
    return absl::StrJoin(attributes, " ", [](std::string* out, const auto& attr) {
               out->append(attr->string());
           }) +
           " ";
}

std::string parameter_type_string(const ParameterType& parameter) {
    switch (parameter.type) {
        case ParameterType::Type::Required: {
            const auto& required = std::get<std::shared_ptr<Required>>(parameter.value);
            return absl::StrFormat("%s: %s", required->name->string(),
                                   mono_type_string(*required->monotype));
        }
        case ParameterType::Type::Optional: {
            const auto& optional = std::get<std::unique_ptr<Optional>>(parameter.value);
            return absl::StrFormat("?%s: %s", optional->name->string(),
                                   mono_type_string(*optional->monotype));
        }
        case ParameterType::Type::Pipe: {
            const auto& pipe = std::get<std::unique_ptr<Pipe>>(parameter.value);
            return absl::StrFormat("<-%s: %s", pipe->name->string(),
                                   mono_type_string(*pipe->monotype));
        }
    }
    pl::assume_unreachable();
}

std::string mono_type_string(const MonoType& mono) {
    switch (mono.type) {
        case MonoType::Type::Tvar:
            return std::get<std::unique_ptr<TvarType>>(mono.value)->name->string();
        case MonoType::Type::Basic:
            return std::get<std::unique_ptr<NamedType>>(mono.value)->name->string();
        case MonoType::Type::Array:
            return absl::StrFormat("[%s]",
                                   mono_type_string(*std::get<std::unique_ptr<ArrayType>>(mono.value)
                                                         ->element));
        case MonoType::Type::Stream:
            return absl::StrFormat("stream[%s]",
                                   mono_type_string(*std::get<std::unique_ptr<StreamType>>(mono.value)
                                                         ->element));
        case MonoType::Type::Vector:
            return absl::StrFormat("vector[%s]",
                                   mono_type_string(*std::get<std::unique_ptr<VectorType>>(mono.value)
                                                         ->element));
        case MonoType::Type::Dict: {
            const auto& dict = std::get<std::unique_ptr<DictType>>(mono.value);
            return absl::StrFormat("[%s:%s]", mono_type_string(*dict->key),
                                   mono_type_string(*dict->val));
        }
        case MonoType::Type::Dynamic:
            return "dynamic";
        case MonoType::Type::Record: {
            const auto& record = std::get<std::unique_ptr<RecordType>>(mono.value);
            std::vector<std::string> props;
            for (const auto& property : record->properties) {
                props.emplace_back(
                    absl::StrFormat("%s: %s", property->name->string(),
                                    mono_type_string(*property->monotype)));
            }
            if (record->tvar) {
                if (props.empty()) {
                    return absl::StrFormat("{%s with}", record->tvar->string());
                }
                return absl::StrFormat("{%s with %s}", record->tvar->string(),
                                       absl::StrJoin(props, ", "));
            }
            return absl::StrFormat("{%s}", absl::StrJoin(props, ", "));
        }
        case MonoType::Type::Function: {
            const auto& function = std::get<std::unique_ptr<FunctionType>>(mono.value);
            std::vector<std::string> params;
            for (const auto& parameter : function->parameters) {
                params.emplace_back(parameter_type_string(*parameter));
            }
            return absl::StrFormat("(%s) => %s", absl::StrJoin(params, ", "),
                                   mono_type_string(*function->monotype));
        }
        case MonoType::Type::Label:
            return std::get<std::unique_ptr<LabelLit>>(mono.value)->string();
    }
    pl::assume_unreachable();
}

} // namespace

std::string Attribute::string() const {
    if (params.empty()) {
        return "@" + name;
    }
    auto values = absl::StrJoin(params, ", ", [](std::string* out, const auto& param) {
        out->append(param->value->string());
    });
    return absl::StrCat("@", name, "(", values, ")");
}

std::string PackageClause::string() const {
    return attributes_prefix(attributes) + "package " + name->string();
}

std::string ImportDeclaration::string() const {
    if (alias) {
        return attributes_prefix(attributes) + "import " + alias->string() + " " + path->string();
    }
    return attributes_prefix(attributes) + "import " + path->string();
}

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
    auto props = absl::StrJoin(params, ", ", [](std::string* out, const auto& p) {
        out->append(p->string());
    });
    return absl::StrFormat("(%s) => { %s }", props, body->string());
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
    return absl::StrFormat("%s.%s", object->string(), property->string());
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
    if (!value) {
        return key->string();
    }
    return absl::StrFormat("%s: %s", key->string(), value->string());
}

std::string PipeExpr::string() const {
    std::stringstream ss;
    ss << "|> " << call->string() << " { " << argument->string() << " } ";
    return ss.str();
}

std::string CallExpr::string() const {
    auto args = absl::StrJoin(arguments, ", ", [](std::string* out, const auto& e) {
        out->append(e->string());
    });
    return absl::StrFormat("%s: ( %s )", callee->string(), args);
}

std::string PropertyKey::string() const {
    switch (type) {
        case Type::Identifier:
            return std::get<std::unique_ptr<Identifier>>(key)->string();
        case Type::StringLiteral:
            return std::get<std::unique_ptr<StringLit>>(key)->string();
    }
    __builtin_unreachable();
}
std::string Block::string() const {
    return absl::StrFormat("{%s}", absl::StrJoin(body, "; ", [](std::string* out, const auto& b) {
                               out->append(b->string());
                           }));
}

std::string StringLit::string() const { return value; }

std::string FloatLit::string() const { return absl::StrFormat("%.4f", value); }

std::string IntegerLit::string() const { return absl::StrFormat("%d", value); }

std::string Duration::string() const { return absl::StrFormat("%d%s", magnitude, unit); }

std::string DurationLit::string() const {
    return absl::StrJoin(values, ", ", [](std::string* out, const auto& d) {
        out->append(d->string());
    });
}

std::string ReturnStmt::string() const { return absl::StrFormat("return %s", argument->string()); }

std::string LogicalExpr::string() const {
    return absl::StrFormat("%s %s %s", left->string(), op_string(op), right->string());
}

std::string WithSource::string() const { return source ? source->string() : ""; }

std::string ObjectExpr::string() const {
    auto props = absl::StrJoin(properties, ", ", [](std::string* out, const auto& p) {
        out->append(p->string());
    });
    if (with) {
        if (props.empty()) {
            return absl::StrFormat("{ %s with }", with->string());
        }
        return absl::StrFormat("{ %s with %s }", with->string(), props);
    }
    return absl::StrFormat("{ %s }", props);
}

std::string IndexExpr::string() const {
    return absl::StrFormat("%s[%s]", array->string(), index->string());
}

std::string BinaryExpr::string() const {
    return absl::StrFormat("%s %s %s", left->string(), op_string(op), right->string());
}

std::string UnaryExpr::string() const {
    return absl::StrFormat("%s %s", op_string(op), argument->string());
}
std::string ConditionalExpr::string() const {
    return absl::StrFormat("if %s then %s else %s", test->string(), consequent->string(),
                           alternate->string());
}
std::string StringExpr::string() const {
    return "\"" + absl::StrJoin(parts, "", [](std::string* out, const auto& p) {
               out->append(p->string());
           }) +
           "\"";
}
std::string StringExprPart::string() const {
    switch (type) {
        case Type::Text:
            return std::get<std::unique_ptr<TextPart>>(part)->string();
        case Type::Interpolated:
            return std::get<std::unique_ptr<InterpolatedPart>>(part)->string();
    }
    pl::assume_unreachable();
}
std::string TextPart::string() const { return value; }
std::string InterpolatedPart::string() const { return "${" + expression->string() + "}"; }
std::string ParenExpr::string() const { return "(" + expression->string() + ")"; }
std::string BooleanLit::string() const { return value ? "true" : "false"; }
std::string DateTimeLit::string() const {
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &value);
    return buf;
}
std::string RegexpLit::string() const { return "/" + value + "/"; }
std::string PipeLit::string() const { return "<-"; }
std::string LabelLit::string() const { return "." + value; }
std::string BadExpr::string() const { return text; }
std::string UintLit::string() const { return absl::StrCat(value, "u"); }

std::string Statement::string() const {
    std::stringstream ss;
    ss << attributes_prefix(attributes);
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

std::string Identifier::string() const { return name; }

std::string DictExpr::string() const {
    return absl::StrFormat("[ %s ]",
                           absl::StrJoin(elements, ", ", [](std::string* out, const auto& e) {
                               out->append(e->key->string());
                               out->append(": ");
                               out->append(e->val->string());
                           }));
}

std::string BadStmt::string() const { return text; }

std::string TestCaseStmt::string() const {
    if (extends) {
        return absl::StrFormat("testcase %s extends %s %s", id->string(), extends->string(),
                               block->string());
    }
    return absl::StrFormat("testcase %s %s", id->string(), block->string());
}

std::string BuiltinStmt::string() const {
    std::string type_expr = mono_type_string(*ty->monotype);
    if (!ty->constraints.empty()) {
        auto constraints = absl::StrJoin(ty->constraints, ", ", [](std::string* out, const auto& c) {
            out->append(c->tvar->string());
            out->append(": ");
            out->append(absl::StrJoin(c->kinds, " + ", [](std::string* kind_out, const auto& kind) {
                kind_out->append(kind->string());
            }));
        });
        type_expr += " where " + constraints;
    }
    return absl::StrFormat("builtin %s: %s", id->string(), type_expr);
}

} // namespace pl
