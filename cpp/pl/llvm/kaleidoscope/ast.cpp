// Copyright (c) 2024 The Authors. All rights reserved.
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

#include "cpp/pl/llvm/kaleidoscope/ast.h"
#include "cpp/pl/llvm/kaleidoscope/laxer.h"

#include <map>

namespace pl::llvm {

static std::map<char, int> binop_precedence = {
    {'<', 10},
    {'+', 20},
    {'-', 20},
    {'*', 40},
};

static int get_tok_precedence() {
    if (isascii(curtok()) == 0) {
        return -1;
    }
    int tok_prec = binop_precedence[curtok()];
    if (tok_prec <= 0) {
        return -1;
    }
    return tok_prec;
}

static std::unique_ptr<ExprAST> log_error(const char* str) {
    ::fprintf(stderr, "Error: %s\n", str);
    return nullptr;
}

static std::unique_ptr<PrototypeAST> log_error_p(const char* str) {
    log_error(str);
    return nullptr;
}

// numberexpr ::= number
std::unique_ptr<ExprAST> parse_number_expr() {
    auto result = std::make_unique<NumberExprAST>(get_num_val());
    get_next_token();
    return result;
}

// parenexpr ::= '(' expression ')'
std::unique_ptr<ExprAST> parse_paren_expr() {
    get_next_token();
    auto v = parse_expression();
    if (!v) {
        return nullptr;
    }
    if (curtok() != ')') {
        return log_error("expected ')'");
    }
    get_next_token();
    return v;
}

// identifierexpr
//     ::= identifier
//     ::= identifier '(' expression ')'
std::unique_ptr<ExprAST> parse_identifier_expr() {
    std::string id_name = identifier_string();
    get_next_token();
    if (curtok() != '(') {
        // sample variable ref
        return std::make_unique<VariableExprAST>(id_name);
    }
    // call
    get_next_token();
    std::vector<std::unique_ptr<ExprAST>> args;
    if (curtok() != ')') {
        while (true) {
            if (auto arg = parse_expression()) {
                args.push_back(std::move(arg));
            } else {
                return nullptr;
            }
            if (curtok() == ')') {
                break;
            }
            if (curtok() != ',') {
                return log_error("Expected ')' or ',' on argument list");
            }
            get_next_token();
        }
    }

    // eat ')'
    get_next_token();

    return std::make_unique<CallExprAST>(id_name, std::move(args));
}

std::unique_ptr<ExprAST> parse_primary() {
    switch (curtok()) {
    case tok_identifier:
        return parse_identifier_expr();
    case tok_number:
        return parse_number_expr();
    case '(':
        return parse_paren_expr();
    default:
        return log_error("unknown token when expecting an expression");
    }
}

// expression ::= primary binoprhs
std::unique_ptr<ExprAST> parse_expression() {
    auto lhs = parse_primary();
    if (!lhs) {
        return nullptr;
    }
    return parse_bin_op_rhs(0, std::move(lhs));
}

// binoprhs ::= ('+' primary)*
std::unique_ptr<ExprAST> parse_bin_op_rhs(int expr_prec, std::unique_ptr<ExprAST> lhs) {
    while (true) {
        int tok_prec = get_tok_precedence();
        if (tok_prec < expr_prec) {
            return lhs;
        }
        int bin_op = curtok();
        get_next_token();
        auto rhs = parse_primary();
        if (!rhs) {
            return nullptr;
        }
        int nex_prec = get_tok_precedence();
        if (tok_prec < nex_prec) {
            rhs = parse_bin_op_rhs(tok_prec, std::move(rhs));
            if (!rhs) {
                return nullptr;
            }
        }
        lhs = std::make_unique<BinaryExprAST>(bin_op, std::move(lhs), std::move(rhs));
    }
}

// prototype
//   ::= id '(' id* ')'
std::unique_ptr<PrototypeAST> parse_prototype() {
    if (curtok() != tok_identifier) {
        return log_error_p("Expected function name in prototype");
    }
    std::string func_name = identifier_string();
    get_next_token();
    if (curtok() != '(') {
        return log_error_p("Expected '(' in prototype");
    }
    std::vector<std::string> arg_names;
    while (get_next_token() == tok_identifier) {
        arg_names.push_back(identifier_string());
    }
    // success
    get_next_token();
    return std::make_unique<PrototypeAST>(func_name, std::move(arg_names));
}

// definition ::= 'def' prototype expression
std::unique_ptr<FunctionAST> parse_definition() {
    get_next_token();
    auto proto = parse_prototype();
    if (!proto) {
        return nullptr;
    }
    if (auto e = parse_expression()) {
        return std::make_unique<FunctionAST>(std::move(proto), std::move(e));
    }
    return nullptr;
}

// toplevelexpr ::= expression
std::unique_ptr<FunctionAST> parse_top_level_expr() {
    if (auto e = parse_expression()) {
        auto proto = std::make_unique<PrototypeAST>("__anon_expr", std::vector<std::string>());
        return std::make_unique<FunctionAST>(std::move(proto), std::move(e));
    }
    return nullptr;
}

// external ::= 'extern' prototype
std::unique_ptr<PrototypeAST> parse_extern() {
    get_next_token();
    return parse_prototype();
}

void handle_definition() {
    if (parse_definition()) {
        fprintf(stderr, "parsed a function definition.\n");
    } else {
        get_next_token();
    }
}

void handle_extern() {
    if (parse_extern()) {
        fprintf(stderr, "parsed an extern.\n");
    } else {
        get_next_token();
    }
}

void handle_top_level_expression() {
    if (parse_top_level_expr()) {
        fprintf(stderr, "parsed a top-level expr.\n");
    } else {
        get_next_token();
    }
}

void run() {
    for (;;) {
        fprintf(stderr, "ready> ");
        switch (curtok()) {
        case tok_eof:
            return;
        case ';':
            get_next_token();
            break;
        case tok_def:
            handle_definition();
            break;
        case tok_extern:
            handle_extern();
            break;
        default:
            handle_top_level_expression();
            break;
        }
    }
}

} // namespace pl::llvm
