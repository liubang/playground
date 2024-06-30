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

#pragma once

#include <map>
#include <memory>
#include <vector>

#include "token.h"

namespace pl {
class Scanner {

public:
    Scanner(const char* data, size_t len)
        : data_(data),
          data_len_(len),
          ps_(data),
          p_(data),
          pe_(data + len),
          eof_(data + len),
          last_newline_(data),
          cur_line_(1),
          checkpoint_(data),
          checkpoint_line_(1),
          checkpoint_last_newline_(data),
          token_(TokenType::Illegal) {}

    std::unique_ptr<Token> scan() { return scan_with_comments(0); }
    std::unique_ptr<Token> scan_with_regex() { return scan_with_comments(1); }
    std::unique_ptr<Token> scan_with_expr() { return scan_with_comments(2); }
    std::unique_ptr<Token> scan_with_comments(int32_t mode);

    /**
     * unread will reset the Scanner to go back to the location before the last scan_with_regex or
     * scan call. If either of the scan_with_regex methods returned an EOF token, a call to unread
     * will not unread the discarded whitespace.
     */
    void unread() {
        p_ = checkpoint_;
        cur_line_ = checkpoint_line_;
        last_newline_ = checkpoint_last_newline_;
    }

    /**
     * Get the offset of a position
     */
    uint32_t offset(const Position& pos) {
        if (positions_.count(pos) == 0) {
            // TODO(liubang): error handler
            return UINT32_MAX;
        }
        return positions_.at(pos);
    }

    /**
     * Append a comment to the current scanner
     */
    void set_comments(const std::vector<std::shared_ptr<Comment>>& t) {
        comments_.insert(comments_.end(), t.begin(), t.end());
    }

private:
    std::unique_ptr<Token> scan(int32_t mode);
    std::unique_ptr<Token> get_eof_token();

private:
    const char* data_;
    size_t data_len_;
    const char* ps_;
    const char* p_;
    const char* pe_;
    const char* eof_;
    const char* last_newline_;
    int32_t cur_line_;
    const char* checkpoint_;
    int32_t checkpoint_line_;
    const char* checkpoint_last_newline_;
    TokenType token_;
    std::map<Position, uint32_t> positions_;
    std::vector<std::shared_ptr<Comment>> comments_;
};
} // namespace pl
