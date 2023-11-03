#pragma once

#include <memory>
#include <set>
#include <sstream>

#include "scanner.h"
#include "token.h"

namespace pl {

class Parser {
public:
    Parser(const std::string& input)
        : scanner_(std::make_unique<Scanner>(input.data(), input.size())),
          source_(input),
          fname_(""),
          depth_(0) {}

private:
    // scan will read the next token from the Scanner. If peek has been used,
    // this will return the peeked token and consume it.
    std::unique_ptr<Token> scan() {
        if (token_) {
            return std::move(token_);
        }
        return scanner_->scan();
    }

    // peek will read the next token from the Scanner and then buffer it.
    // It will return information about the token.
    const Token* peek() {
        if (token_) {
            return token_.get();
        }
        token_ = scanner_->scan();
        return token_.get();
    }

    // peek_with_regex is the same as peek, except that the scan step will allow scanning regexp
    // tokens.
    const Token* peek_with_regex() {
        if (token_ && token_->tok == TokenType::Div) {
            scanner_->set_comments(token_->comments);
            token_.reset();
            scanner_->unread();
        }
        if (!token_) {
            token_ = scanner_->scan_with_regex();
        }
        return token_.get();
    }

    // consume will consume a token that has been retrieve using peek.
    // This will panic if a token has not been buffered with peek.
    std::unique_ptr<Token> consume() {
        if (token_) {
            return std::move(token_);
        }
        // TODO: handler error
        return nullptr;
    }

    // expect will check if the next token is `exp` and error if it is not in either case the token
    // is consumed and returned
    std::unique_ptr<Token> expect(TokenType exp) { return expect_one_of(std::set<TokenType>{exp}); }
    std::unique_ptr<Token> expect_one_of(const std::set<TokenType>& exp) {
        auto t = scan();
        if (exp.count(t->tok) > 0) {
            return t;
        }

        auto exp_to_string = [&exp]() -> std::string {
            auto len = exp.size();
            switch (len) {
            case 0:
                return "";
            case 1:
                return token_to_string(*(exp.begin()));
            default:
                bool first = true;
                std::stringstream ss;
                for (auto t : exp) {
                    if (!first) {
                        ss << " or ";
                    } else {
                        first = false;
                    }
                    ss << token_to_string(t);
                }
                return ss.str();
            }
        };

        if (t->tok == TokenType::Eof) {
            errs_.emplace_back("expected " + exp_to_string() + ", got EOF");
        } else {
            std::stringstream ss;
            ss << "expected " << exp_to_string() << ", got " << token_to_string(t->tok) << "("
               << t->lit << ") at " << t->start_pos;
            errs_.emplace_back(ss.str());
        }
        return t;
    }
    // If `exp` is not the next token this will record an error and continue without consuming the
    // token so that the next step in the parse may use it
    std::unique_ptr<Token> expect_or_skip(TokenType exp) {
        auto t = scan();
        if (t->tok == exp) {
            return t;
        }
        token_ = std::move(t);
        auto ret = std::unique_ptr<Token>();
        ret->start_offset = token_->start_offset;
        ret->end_offset = token_->end_offset;
        ret->start_pos = token_->start_pos;
        ret->end_pos = token_->end_pos;
        if (t->tok == TokenType::Eof) {
            errs_.emplace_back("expected " + token_to_string(exp) + ", got EOF");
            auto ret = std::unique_ptr<Token>();
            ret->tok = token_->tok;
            ret->comments = token_->comments;
        } else {
            std::stringstream ss;
            ss << "expected " << token_to_string(exp) << ", got " << token_to_string(t->tok) << "("
               << t->lit << ") at " << t->start_pos;
            errs_.emplace_back(ss.str());
            ret->tok = TokenType::Illegal;
        }
        return ret;
    }

    // open will open a new block. It will expect that the next token is the sater token and mark
    // that we expect the end token in the future.
    std::unique_ptr<Token> open(TokenType start, TokenType end) {
        auto t = expect(start);
        if (blocks_.find(end) != blocks_.end()) {
            blocks_[end] = blocks_[end] + 1;
        } else {
            blocks_.insert(std::make_pair(end, 1));
        }
        return t;
    }

    // more will check if we should continue reading tokens for the current block. This is true when
    // the next token is not EOF and the next token is also not one that would close a block.
    bool more() {
        auto t_tok = peek()->tok;
        if (t_tok == TokenType::Eof) {
            return false;
        }
        return blocks_.find(t_tok) == blocks_.end() || blocks_[t_tok] == 0;
    }

    std::unique_ptr<Token> close() {}

private:
    std::unique_ptr<Scanner> scanner_;
    std::unique_ptr<Token> token_;
    std::vector<std::string> errs_;
    std::map<TokenType, int32_t> blocks_;
    std::string source_;
    std::string fname_;
    uint32_t depth_;
};

} // namespace pl
