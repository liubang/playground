//=====================================================================
//
// ast.cpp -
//
// Created by liubang on 2023/11/27 19:06
// Last Modified: 2023/11/27 19:06
//
//=====================================================================
#include "ast.h"

#include "absl/strings/str_join.h"

namespace pl {

std::string ArrayItem::string() const { return "expression: " + expression->string(); }

std::string ExprStmt::string() const { return expression->string(); }

std::string ArrayExpr::string() const {
    std::stringstream ss;
    ss << absl::StrJoin(lbrack, " ", [](std::string* out, const auto& c) {
        out->append(c->text);
    });
    ss << absl::StrJoin(elements, ", ", [](std::string* out, const auto& e) {
        out->append(e->string());
    });
    ss << absl::StrJoin(lbrack, " ", [](std::string* out, const auto& c) {
        out->append(c->text);
    });

    return ss.str();
}

std::string AttributeParam::string() const { return "value: " + value->string(); }

std::string VariableAssgn::string() const { return id->string() + " = " + init->string(); }

std::string OptionStmt::string() const { return assignment->string(); }

// TODO
std::string Assignment::string() const { return ""; }

// TODO
std::string FunctionExpr::string() const { return ""; }

} // namespace pl
