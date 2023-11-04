#pragma once

#include <memory>
#include <set>
#include <sstream>
#include <utility>

#include "ast.h"
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

    // Parses a file of Flux source code, returning a Package
    std::unique_ptr<Package> parse_single_package(const std::string& pkgpath,
                                                  const std::string& fname) {
        std::shared_ptr<File> ast_file = parse_file(fname);
        auto package = std::make_unique<Package>();
        package->package = ast_file->package->name->name;
        package->base = ast_file;
        package->path = pkgpath;
        package->files.emplace_back(ast_file);
        return package;
    }

    // Parses a file of Flux source code, returning a File
    std::unique_ptr<File> parse_file(const std::string& fname) {
        fname_ = fname;
        auto start_pos = peek()->start_pos;
        auto end = Position::invalid();
        auto inner_attributes = parse_attribute_inner_list();
        auto pkg = parse_package_clause(&inner_attributes);
        if (pkg) {
            end = pkg->base->location.end;
        }
        auto imports = parse_import_list(&inner_attributes);
        if (!imports.empty()) {
            end = imports.rbegin()->get()->location.end;
        }
        auto body = parse_statement_list(&inner_attributes);
        if (!inner_attributes.empty()) {
            // We have left over attributes from the beginning of the file.
            auto badstmt = std::make_shared<BadStmt>();
            badstmt->base = base_node_from_others(inner_attributes[0]->base.get(),
                                                  inner_attributes.rbegin()->get()->base.get());
            badstmt->text = "extra attributes not associated with anything";
            body.emplace_back(std::make_shared<Statement>(Statement::Type::BadStatement, badstmt));
        }
        if (!body.empty()) {
            end = body.rbegin()->get()->base()->location.end;
        }
        auto eof = peek()->comments;
        auto ret = std::make_unique<File>();
        ret->base = std::make_shared<BaseNode>();
        ret->base->location = source_location(start_pos, end);
        ret->name = fname_;
        ret->metadata = METADATA;
        ret->package = std::move(pkg);
        ret->body = body;
        ret->imports = imports;
        ret->eof = eof;
        return ret;
    }

private:
    constexpr static char METADATA[] = "parser-type=rust";

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
        auto ret = std::make_unique<Token>();
        ret->start_offset = token_->start_offset;
        ret->end_offset = token_->end_offset;
        ret->start_pos = token_->start_pos;
        ret->end_pos = token_->end_pos;
        if (t->tok == TokenType::Eof) {
            errs_.emplace_back("expected " + token_to_string(exp) + ", got EOF");
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

    // close will close a block that was opened using open.
    //
    // This function will always decrement the block count for the end token.
    //
    // If the next token is the end token, then this will consume the token and return the pos and
    // lit for the token. Otherwise, it will return NoPos.
    std::unique_ptr<Token> close(TokenType end) {
        if (end == TokenType::Eof) {
            return scan();
        }
        if (blocks_.find(end) == blocks_.end()) {
            // TODO: error handler
            return nullptr;
        }
        blocks_[end] = blocks_[end] - 1;
        const auto* token = peek();
        if (token->tok == end) {
            return consume();
        }
        errs_.emplace_back("expected " + token_to_string(end) + ", got " +
                           token_to_string(token->tok));
        auto ret = std::make_unique<Token>();
        ret->tok = token->tok;
        ret->lit = token->lit;
        ret->start_pos = token->start_pos;
        ret->end_pos = token->end_pos;
        ret->start_offset = token->start_offset;
        ret->end_offset = token->end_offset;
        return ret;
    }

    std::unique_ptr<BaseNode> base_node(SourceLocation location) {
        auto ret = std::make_unique<BaseNode>();
        ret->location = std::move(location);
        ret->errors = errs_;
        return ret;
    }

    std::unique_ptr<BaseNode> base_node_from_token(const Token* token) {
        auto base = base_node_from_tokens(token, token);
        base->comments = token->comments;
        return base;
    }

    std::unique_ptr<BaseNode> base_node_from_tokens(const Token* start, const Token* end) {
        return base_node(source_location(start->start_pos, end->end_pos));
    }

    std::unique_ptr<BaseNode> base_node_from_other_start(const BaseNode* start, const Token* end) {
        return base_node(source_location(start->location.start, end->end_pos));
    }

    std::unique_ptr<BaseNode> base_node_from_other_end(const Token* start, const BaseNode* end) {
        return base_node(source_location(start->start_pos, end->location.end));
    }

    std::unique_ptr<BaseNode> base_node_from_other_end_c(const Token* start,
                                                         const BaseNode* end,
                                                         const Token* comments_from) {
        auto base = base_node(source_location(start->start_pos, end->location.end));
        base->comments = comments_from->comments;
        return base;
    }

    std::unique_ptr<BaseNode>
    base_node_from_other_end_c_a(const Token* start,
                                 const BaseNode* end,
                                 const Token* comments_from,
                                 const std::vector<std::shared_ptr<Attribute>>& attributes) {
        auto base = base_node(source_location(start->start_pos, end->location.end));
        base->comments = comments_from->comments;
        base->attributes = attributes;
        return base;
    }

    std::unique_ptr<BaseNode> base_node_from_others_c(const BaseNode* start,
                                                      const BaseNode* end,
                                                      const Token* comments_from) {
        auto base = base_node_from_pos(start->location.start, end->location.end);
        base->comments = comments_from->comments;
        return base;
    }

    std::unique_ptr<BaseNode> base_node_from_others(const BaseNode* start, const BaseNode* end) {
        return base_node_from_pos(start->location.start, end->location.end);
    }

    std::unique_ptr<BaseNode> base_node_from_pos(const Position& start, const Position& end) {
        return base_node(source_location(start, end));
    }

    SourceLocation source_location(const Position& start, const Position& end) {
        if (!start.is_valid() || !end.is_valid()) {
            return SourceLocation::_default();
        }
        SourceLocation ret;
        ret.file = fname_;
        ret.start = start;
        ret.end = end;
        auto s = scanner_->offset(start);
        auto e = scanner_->offset(end);
        ret.source = std::string(source_.data() + s, (e - s));
        return ret;
    }

    std::vector<std::shared_ptr<Attribute>> parse_attribute_inner_list() {}

    std::unique_ptr<PackageClause>
    parse_package_clause(std::vector<std::shared_ptr<Attribute>>* attributes) {}

    std::vector<std::shared_ptr<ImportDeclaration>>
    parse_import_list(std::vector<std::shared_ptr<Attribute>>* attributes) {}

    std::vector<std::shared_ptr<Statement>>
    parse_statement_list(std::vector<std::shared_ptr<Attribute>>* attributes) {}

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
