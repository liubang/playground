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

#include "scanner.h"

namespace pl {

extern uint32_t real_scan(int32_t mode,
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

TokenPtr Scanner::scan_with_comments(int32_t mode) {
    TokenPtr token;
    for (;;) {
        token = scan(mode);
        if (token->tok != TokenType::COMMENT) {
            break;
        }
        comments_.emplace_back(std::make_shared<Comment>(token->lit));
    }
    token->comments.insert(token->comments.end(), comments_.begin(), comments_.end());
    return token;
}

TokenPtr Scanner::scan(int32_t mode) {
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
        real_scan(mode, &p_, ps_, pe_, eof_, &last_newline_, cur_line_, token_, token_start,
                  token_start_line, token_start_col, token_end, token_end_line, token_end_col);
    TokenPtr t;
    if (err != 0) {
        // TODO(liubang):
        t = get_eof_token();
    }

    if (token_ == TokenType::ILLEGAL && p_ == eof_) {
        t = get_eof_token();
    } else {
        t = std::make_unique<Token>();
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

TokenPtr Scanner::get_eof_token() {
    uint32_t column = eof_ - last_newline_ + 1;
    auto token = std::make_unique<Token>();
    token->tok = TokenType::Eof;
    token->lit = "";
    token->start_offset = data_len_;
    token->end_offset = data_len_;
    token->start_pos = Position(cur_line_, column);
    token->end_pos = Position(cur_line_, column);
    return token;
}
} // namespace pl
