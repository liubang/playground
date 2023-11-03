#pragma once

#include "scanner.h"
#include "token.h"

#include <memory>
#include <set>

namespace pl {

class Parser {
public:
    Parser(const std::string& input)
        : scanner_(std::make_unique<Scanner>(input.data(), input.size())),
          source_(input),
          fname_(""),
          depth_(0) {}

    std::shared_ptr<Token> scan() {
        if (token_) {
            return token_;
        }
        return scanner_->scan();
    }

    std::shared_ptr<Token> peek() {
        if (token_) {
            return token_;
        }
        auto token = scanner_->scan();
        token_ = std::move(token);
        return token_;
    }

    // TODO:
    std::shared_ptr<Token> peek_with_regex() { return nullptr; }

    std::shared_ptr<Token> consume() {
        if (token_) {
            return token_;
        }
        // TODO: handler error
        return nullptr;
    }

    std::shared_ptr<Token> expect(TokenType exp) { expect_one_of(std::set<TokenType>{exp}); }
    std::shared_ptr<Token> expect_one_of(std::set<TokenType> exp) {
        auto t = scan();
        if (exp.count(t->tok) > 0) {
            return t;
        }

        // TODO: 完善错误信息
        if (t->tok == TokenType::Eof) {
            errs_.emplace_back("expected , got EOF");
        } else {
            errs_.emplace_back("");
        }
    }
    std::shared_ptr<Token> expect_or_skip(TokenType exp) {
        auto t = scan();
        if (t->tok == exp) {
            return t;
        }
        if (t->tok == TokenType::Eof) {
            errs_.emplace_back("expected , got EOF");
        } else {
            // TODO: get position of current token
            auto ret = std::make_shared<Token>();
            ret->tok = TokenType::Illegal;
            return ret;
        }
    }

private:
    std::unique_ptr<Scanner> scanner_;
    std::shared_ptr<Token> token_;
    std::vector<std::string> errs_;
    std::map<TokenType, int32_t> blocks_;
    std::string source_;
    std::string fname_;
    uint32_t depth_;
};

} // namespace pl
