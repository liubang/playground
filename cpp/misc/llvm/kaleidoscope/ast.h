//=====================================================================
//
// ast.h -
//
// Created by liubang on 2023/09/17 23:26
// Last Modified: 2023/09/17 23:26
//
//=====================================================================

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pl::llvm {

class ExprAST {
public:
    virtual ~ExprAST() = default;
};

class NumberExprAST : public ExprAST {
public:
    NumberExprAST(double val) : val_(val) {}

    [[nodiscard]] double val() const { return val_; }

private:
    double val_;
};

class VariableExprAST : public ExprAST {
public:
    VariableExprAST(std::string name) : name_(std::move(name)) {}

    [[nodiscard]] const std::string& name() const { return name_; }

private:
    std::string name_;
};

class BinaryExprAST : public ExprAST {
public:
    BinaryExprAST(char op, std::unique_ptr<ExprAST> lhs, std::unique_ptr<ExprAST> rhs)
        : op_(op), lhs_(std::move(lhs)), rhs_(std::move(rhs)) {}

    [[nodiscard]] char op() const { return op_; }

private:
    char op_;
    std::unique_ptr<ExprAST> lhs_;
    std::unique_ptr<ExprAST> rhs_;
};

class CallExprAST : public ExprAST {
public:
    CallExprAST(std::string callee, std::vector<std::unique_ptr<ExprAST>> args)
        : callee_(std::move(callee)), args_(std::move(args)) {}

private:
    std::string callee_;
    std::vector<std::unique_ptr<ExprAST>> args_;
};

class PrototypeAST {
public:
    PrototypeAST(std::string name, std::vector<std::string> args)
        : name_(std::move(name)), args_(std::move(args)) {}

    [[nodiscard]] const std::string& name() const { return name_; }

private:
    std::string name_;
    std::vector<std::string> args_;
};

class FunctionAST {
public:
    FunctionAST(std::unique_ptr<PrototypeAST> proto, std::unique_ptr<ExprAST> body)
        : proto_(std::move(proto)), body_(std::move(body)) {}

private:
    std::unique_ptr<PrototypeAST> proto_;
    std::unique_ptr<ExprAST> body_;
};

std::unique_ptr<ExprAST> log_error(const char* str) {
    ::fprintf(stderr, "Error: %s\n", str);
    return nullptr;
}

std::unique_ptr<PrototypeAST> log_error_p(const char* str) {
    log_error(str);
    return nullptr;
}

std::unique_ptr<ExprAST> parse_number_expr();
std::unique_ptr<ExprAST> parse_paren_expr();
std::unique_ptr<ExprAST> parse_expression();
std::unique_ptr<ExprAST> parse_identifier_expr();
std::unique_ptr<ExprAST> parse_primary();
std::unique_ptr<ExprAST> parse_bin_op_rhs(int expr_prec, std::unique_ptr<ExprAST> lhs);

} // namespace pl::llvm
