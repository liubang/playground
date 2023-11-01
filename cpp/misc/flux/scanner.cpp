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
#include <memory>
#include <string>
#include <vector>

#include "token.h"

struct Position {
    uint32_t line;
    uint32_t column;

    Position() = default;
    Position(uint32_t line, uint32_t column) : line(line), column(column) {}
};

struct Comment {
    std::string text;
    Comment() = default;
    Comment(std::string text) : text(std::move(text)) {}
};

struct Token {
    TokenType tok;
    std::string lit;
    uint32_t start_offset;
    uint32_t end_offset;
    std::shared_ptr<Position> start_pos;
    std::shared_ptr<Position> end_pos;
    std::vector<std::shared_ptr<Comment>> comments;
};

// TODO(liubang): to implement.
extern int real_scan(const char* data,
                     int32_t mode,
                     const char* p,
                     const char* ps,
                     const char* pe,
                     const char* eof,
                     const char*& last_newline,
                     int32_t& cur_line,
                     TokenType& token,
                     int32_t& token_start,
                     int32_t& token_start_line,
                     int32_t& token_start_col,
                     int32_t& token_end,
                     int32_t& token_end_line,
                     int32_t& token_end_col);

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

public:
    std::shared_ptr<Token> scan() { return scan_with_comments(0); }

    std::shared_ptr<Token> scan_with_comments(int32_t mode) {
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

    std::shared_ptr<Token> scan_with_regex() { return scan_with_comments(1); }
    std::shared_ptr<Token> scan_with_expr() { return scan_with_comments(2); }

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
    uint32_t offset(std::shared_ptr<Position> pos) {
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
    std::shared_ptr<Token> scan(int32_t mode) {
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

        auto err = real_scan(data_, mode, p_, ps_, pe_, eof_, last_newline_, cur_line_, token_,
                             token_start, token_start_line, token_start_col, token_end,
                             token_end_line, token_end_col);
        std::shared_ptr<Token> t;
        if (err != 0) {
            // TODO(liubang):
        }
        if (token_ == TokenType::Illegal && p_ == eof_) {
            t = get_eof_token();
        } else {
            t->tok = token_;
            t->lit = std::string(data_ + token_start, token_end);
            t->start_offset = token_start;
            t->end_offset = token_end;
            t->start_pos = std::make_shared<Position>(token_start_line, token_start_col);
            t->end_pos = std::make_shared<Position>(token_end_line, token_end_col);
        }
        positions_[t->start_pos] = t->start_offset;
        positions_[t->end_pos] = t->end_offset;

        return t;
    }

    std::shared_ptr<Token> get_eof_token() {
        uint32_t column = eof_ - last_newline_ + 1;
        auto token = std::make_shared<Token>();
        token->tok = TokenType::Eof;
        token->lit = "";
        token->start_offset = data_len_;
        token->end_offset = data_len_;
        token->start_pos = std::make_shared<Position>(cur_line_, column);
        token->end_pos = std::make_shared<Position>(cur_line_, column);
        return token;
    }

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
    std::map<std::shared_ptr<Position>, uint32_t> positions_;
    std::vector<std::shared_ptr<Comment>> comments_;
};
