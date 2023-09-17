//=====================================================================
//
// ast.cpp -
//
// Created by liubang on 2023/09/18 00:04
// Last Modified: 2023/09/18 00:04
//
//=====================================================================
#include "cpp/misc/llvm/kaleidoscope/ast.h"
#include "cpp/misc/llvm/kaleidoscope/laxer.h"
#include <map>

namespace pl::llvm {

static std::map<char, int> binop_precedence = {
    {'<', 10},
    {'+', 20},
    {'-', 20},
    {'*', 40},
};

int get_tok_precedence() {
    if (isascii(curtok()) == 0) {
        return -1;
    }
    int tok_prec = binop_precedence[curtok()];
    if (tok_prec <= 0) {
        return -1;
    }
    return tok_prec;
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

} // namespace pl::llvm
