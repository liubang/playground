//=====================================================================
//
// scanner.cpp -
//
// Created by liubang on 2023/11/01 20:26
// Last Modified: 2023/11/01 20:26
//
//=====================================================================

#include "scanner.h"
#include <iostream>

namespace pl {

extern uint32_t real_scan(const char* data,
                          int32_t mode,
                          const char** p,
                          const char* ps,
                          const char* pe,
                          const char* eof,
                          const char** last_newline,
                          int32_t& cur_line,
                          TokenType& token,
                          int32_t& token_start,
                          int32_t& token_start_line,
                          int32_t& token_start_col,
                          int32_t& token_end,
                          int32_t& token_end_line,
                          int32_t& token_end_col);

std::shared_ptr<Token> Scanner::scan_with_comments(int32_t mode) {
    std::shared_ptr<Token> token;
    for (;;) {
        token = scan(mode);
        if (token->tok != TokenType::Comment) {
            break;
        }
        comments_.emplace_back(std::make_shared<Comment>(token->lit));
    }
    token->comments.insert(token->comments.end(), comments_.begin(), comments_.end());
    return token;
}

std::shared_ptr<Token> Scanner::scan(int32_t mode) {
    if (p_ == eof_) {
        return get_eof_token();
    }
    checkpoint_ = p_;
    checkpoint_line_ = cur_line_;
    checkpoint_last_newline_ = last_newline_;

    int32_t token_start = 0;
    int32_t token_start_line = 0;
    int32_t token_start_col = 0;
    int32_t token_end = 0;
    int32_t token_end_line = 0;
    int32_t token_end_col = 0;

    auto err =
        real_scan(data_, mode, &p_, ps_, pe_, eof_, &last_newline_, cur_line_, token_, token_start,
                  token_start_line, token_start_col, token_end, token_end_line, token_end_col);
    std::shared_ptr<Token> t;
    if (err != 0) {
        // TODO(liubang):
        t = get_eof_token();
    }

    if (token_ == TokenType::Illegal && p_ == eof_) {
        t = get_eof_token();
    } else {
        t = std::make_shared<Token>();
        t->tok = token_;
        t->lit = std::string(data_ + token_start, token_end - token_start);
        t->start_offset = token_start;
        t->end_offset = token_end;
        t->start_pos = Position(token_start_line, token_start_col);
        t->end_pos = Position(token_end_line, token_end_col);
    }
    positions_[t->start_pos] = t->start_offset;
    positions_[t->end_pos] = t->end_offset;

    return t;
}

std::shared_ptr<Token> Scanner::get_eof_token() {
    uint32_t column = eof_ - last_newline_ + 1;
    auto token = std::make_shared<Token>();
    token->tok = TokenType::Eof;
    token->lit = "";
    token->start_offset = data_len_;
    token->end_offset = data_len_;
    token->start_pos = Position(cur_line_, column);
    token->end_pos = Position(cur_line_, column);
    return token;
}

} // namespace pl
