//=====================================================================
//
// scanner.cpp -
//
// Created by liubang on 2023/11/01 20:26
// Last Modified: 2023/11/01 20:26
//
//=====================================================================
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "token.h"

struct Position {
    uint32_t line;
    uint32_t column;
};

struct Comment {
    std::string text;
    Comment(std::string text) : text(std::move(text)) {}
};

struct Token {
    TokenType tok;
    std::string lit;
    uint32_t start_offset;
    uint32_t end_offset;
    Position start_pos;
    Position end_pos;
    std::vector<Comment> comments;
};

class Scanner {
public:
    Scanner(const char* data, size_t len)
        : data_(data),
          ps_(0),
          p_(0),
          pe_(len),
          eof_(len),
          last_newline_(0),
          cur_line_(1),
          checkpoint_(0),
          checkpoint_line_(1),
          checkpoint_last_newline_(0),
          token_(TokenType::Illegal) {}

public:
    Token scan() { return scan_with_comments(0); }

    Token scan_with_comments(int32_t mode) {
        Token token;
        for (;;) {
            token = scan(mode);
            if (token.tok != TokenType::Comment) {
                break;
            }
            comments_.emplace_back(token.lit);
        }
        token.comments.insert(token.comments.end(), comments_.begin(), comments_.end());
        return token;
    }

    Token scan_with_regex(int32_t mode) { return scan_with_comments(1); }
    Token scan_with_expr(int32_t mode) { return scan_with_comments(2); }

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
    void set_comments(const std::vector<Comment>& t) {
        comments_.insert(comments_.end(), t.begin(), t.end());
    }

private:
    Token scan(int32_t mode) {
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
            scan(data_, mode, p_, ps_, pe_, eof_, last_newline_, cur_line_, token_, token_start,
                 token_start_line, token_start_col, token_end, token_end_line, token_end_col);
        Token t;
        if (err != 0) {
        }
        if (token_ == TokenType::Illegal && p_ == eof_) {
            t = get_eof_token();
        } else {
            t = Token{.tok = token_,
                      .lit = std::string(data_ + token_start, token_end),
                      .start_offset = static_cast<uint32_t>(token_start),
                      .end_offset = static_cast<uint32_t>(token_end),
                      .start_pos =
                          Position{
                              .line = static_cast<uint32_t>(token_start_line),
                              .column = static_cast<uint32_t>(token_start_col),
                          },
                      .end_pos = Position{
                          .line = static_cast<uint32_t>(token_end_line),
                          .column = static_cast<uint32_t>(token_end_col),
                      }};
        }
        positions_[t.start_pos] = t.start_offset;
        positions_[t.end_pos] = t.end_offset;

        return t;
    }

    Token get_eof_token() {
        uint32_t data_len = ::strlen(data_);
        uint32_t column = eof_ - last_newline_ + 1;
        return Token{
            .tok = TokenType::Eof,
            .lit = "",
            .start_offset = data_len,
            .end_offset = data_len,
            .start_pos = Position{.line = static_cast<uint32_t>(cur_line_), .column = column},
            .end_pos = Position{.line = static_cast<uint32_t>(cur_line_), .column = column},
        };
    }

    // TODO(liubang): to implement.
    int scan(const char* data,
             int32_t mode,
             int32_t& p,
             int32_t ps,
             int32_t pe,
             int32_t eof,
             int32_t& last_newline,
             int32_t& cur_line,
             TokenType& token,
             int32_t& token_start,
             int32_t& token_start_line,
             int32_t& token_start_col,
             int32_t& token_end,
             int32_t& token_end_line,
             int32_t& token_end_col) {
        return 0;
    }

private:
    const char* data_;
    int32_t ps_;
    int32_t p_;
    int32_t pe_;
    int32_t eof_;
    int32_t last_newline_;
    int32_t cur_line_;
    int32_t checkpoint_;
    int32_t checkpoint_line_;
    int32_t checkpoint_last_newline_;
    TokenType token_;
    std::map<Position, uint32_t> positions_;
    std::vector<Comment> comments_;
};
