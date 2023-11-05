//=====================================================================
//
// parser.h -
//
// Created by liubang on 2023/11/04 17:26
// Last Modified: 2023/11/04 17:26
//
//=====================================================================
#pragma once

#include <algorithm>
#include <memory>
#include <set>
#include <sstream>
#include <utility>

#include "ast.h"
#include "scanner.h"
#include "strconv.h"
#include "token.h"

namespace pl {

struct TokenError {
    TokenError() = default;
    TokenError(std::unique_ptr<Token> t) : token(std::move(t)) {}
    std::unique_ptr<Token> token;
};

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
        package->base = ast_file->base;
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
            end = imports.rbegin()->get()->base->location.end;
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

    std::vector<std::shared_ptr<Attribute>> parse_attribute_inner_list() {
        auto attributes = std::vector<std::shared_ptr<Attribute>>();
        while (peek()->tok == TokenType::Attribute) {
            attributes.emplace_back(parse_attribute_inner());
        }
        return attributes;
    }

    std::unique_ptr<Attribute> parse_attribute_inner() {
        auto tok = expect(TokenType::Attribute);
        auto lit = tok->lit;
        auto name = std::string(std::find_if(lit.begin(), lit.end(),
                                             [](char c) {
                                                 return c != '@';
                                             }),
                                lit.end());
        return parse_attribute_rest(std::move(tok), name);
    }

    std::unique_ptr<Attribute> parse_attribute_rest(std::unique_ptr<Token> tok,
                                                    const std::string& name) {
        // Parenthesis are optional. No parenthesis means no parameters.
        if (peek()->tok != TokenType::LParen) {
            auto ret = std::make_unique<Attribute>();
            ret->base = base_node_from_token(tok.get());
            ret->name = name;
            return ret;
        }

        open(TokenType::LParen, TokenType::RParen);
        auto params = parse_attribute_params();
        auto end = close(TokenType::RParen);
        auto base = base_node_from_tokens(tok.get(), end.get());
        base->comments = tok->comments;
        auto ret = std::make_unique<Attribute>();
        ret->base = std::move(base);
        ret->name = name;
        ret->params = std::move(params);
        return ret;
    }

    std::vector<std::shared_ptr<AttributeParam>> parse_attribute_params() {
        std::vector<std::shared_ptr<AttributeParam>> params;
        while (more()) {
            auto value = parse_primary_expression();
            auto start_pos = value->base()->location.start;
            auto end_pos = value->base()->location.end;
            std::vector<std::shared_ptr<Comment>> comments;

            if (more()) {
                const auto* t = peek();
                if (t->tok != TokenType::Comma) {
                    errs_.emplace_back("expected comma in attribute parameter list, got " +
                                       token_to_string(t->tok));
                } else {
                    auto tt = consume();
                    end_pos = tt->end_pos;
                    comments = tt->comments;
                }
            }

            auto param = std::make_shared<AttributeParam>();
            param->base = base_node_from_pos(start_pos, end_pos);
            param->value = std::move(value);
            param->comma = comments;
            params.emplace_back(param);
        }
        return params;
    }

    std::unique_ptr<Expression> parse_primary_expression() {
        auto t = peek_with_regex();
        auto ret = std::make_unique<Expression>();
        switch (t->tok) {
        case TokenType::Ident: {
            ret->type = Expression::Type::Identifier;
            ret->expr = parse_identifier();
            break;
        }
        case TokenType::Int: {
            ret->type = Expression::Type::IntegerLit;
            ret->expr = parse_int_literal();
            break;
        }
        case TokenType::Float: {
            TokenError err;
            std::unique_ptr<FloatLit> fl;
            std::tie(fl, err) = parse_float_literal();
            if (fl) {
                ret->type = Expression::Type::FloatLit;
                ret->expr = std::move(fl);
            } else {
                return create_bad_expression(std::move(err.token));
            }
            break;
        }
        case TokenType::String: {
            ret->type = Expression::Type::StringLit;
            ret->expr = parse_string_literal();
            break;
        }
        case TokenType::Quote:
            // TODO:
            break;
        case TokenType::Regex:
            // TODO:
            break;
        case TokenType::Time:
            // TODO:
            break;
        case TokenType::Duration:
            // TODO:
            break;
        case TokenType::PipeReceive:
            // TODO:
            break;
        case TokenType::LBrack:
            // TODO:
            break;
        case TokenType::LBrace:
            // TODO:
            break;
        case TokenType::LParen:
            // TODO:
            break;
        case TokenType::Dot:
            // TODO:
            break;
        default:
            break;
        }
        return ret;
    }

    // TODO:
    std::tuple<std::unique_ptr<StringExpr>, TokenError> parse_string_expression() {
        auto start = expect(TokenType::Quote);
        std::vector<std::shared_ptr<StringExprPart>> parts;
        for (;;) {
            auto t = scanner_->scan_with_expr();
            switch (t->tok) {
            case TokenType::Text: {
                auto value = StrConv::parse_text(t->lit);
                if (value.ok()) {
                    std::shared_ptr<StringExprPart> p = std::make_shared<StringExprPart>();
                    std::shared_ptr<TextPart> tp = std::make_shared<TextPart>();
                    tp->base = base_node_from_token(t.get());
                    tp->value = value.t();
                    p->type = StringExprPart::Type::Text;
                    p->part = tp;
                    parts.emplace_back(p);
                } else {
                    return {nullptr, TokenError(std::move(t))};
                }
                break;
            }
            case TokenType::StringExpr: {
                auto expr = parse_expression();
                auto end = expect(TokenType::RBrace);
                std::shared_ptr<StringExprPart> p = std::make_shared<StringExprPart>();
                std::shared_ptr<InterpolatedPart> ip = std::make_shared<InterpolatedPart>();
                ip->base = base_node_from_tokens(t.get(), end.get());
                ip->expression = std::move(expr);
                p->type = StringExprPart::Type::Interpolated;
                p->part = ip;
                parts.emplace_back(p);
                break;
            }
            case TokenType::Quote: {
                auto string_expr = std::make_unique<StringExpr>();
                string_expr->base = base_node_from_tokens(start.get(), t.get());
                string_expr->parts = std::move(parts);
                return {std::move(string_expr), TokenError()};
            }
            default: {
                auto loc = source_location(t->start_pos, t->end_pos);
                std::stringstream ss;
                ss << "got unexpcted token in string expression " << loc << ": "
                   << token_to_string(t->tok);
                errs_.emplace_back(ss.str());
                auto string_expr = std::make_unique<StringExpr>();
                string_expr->base = base_node_from_tokens(start.get(), t.get());
                return {std::move(string_expr), TokenError()};
            }
            }
        }
    }

    // TODO
    std::unique_ptr<Expression> parse_expression() {}

    std::unique_ptr<Expression> parse_conditional_expression() {
        auto t = peek();
        if (t->tok == TokenType::If) {
            auto if_tok = scan();
            auto test = parse_expression();
            auto then_tok = expect_or_skip(TokenType::Then);
            auto cons = then_tok->tok == TokenType::Then
                            ? parse_expression()
                            : create_placeholder_expression(then_tok.get());
            auto else_tok = expect_or_skip(TokenType::Else);
            auto alt = else_tok->tok == TokenType::Else
                           ? parse_expression()
                           : create_placeholder_expression(else_tok.get());

            auto cond_expr = std::make_shared<ConditionalExpr>();
            cond_expr->base = base_node_from_other_end(t, alt->base().get());
            cond_expr->tk_if = if_tok->comments;
            cond_expr->tk_then = then_tok->comments;
            cond_expr->test = std::move(test);
            cond_expr->consequent = std::move(cons);
            cond_expr->tk_else = else_tok->comments;
            cond_expr->alternate = std::move(alt);
            auto exp = std::make_unique<Expression>();
            exp->type = Expression::Type::ConditionalExpr;
            exp->expr = cond_expr;
            return exp;
        }
        return parse_logical_or_expression();
    }

    std::unique_ptr<Expression> create_placeholder_expression(const Token* tok) {
        auto expr = std::make_unique<Expression>();
        auto bad_expr = std::make_shared<BadExpr>();

        bad_expr->base = std::make_shared<BaseNode>();
        bad_expr->base->location = source_location(tok->start_pos, tok->end_pos);

        expr->type = Expression::Type::BadExpr;
        expr->expr = bad_expr;

        return expr;
    }

    // TODO:
    std::unique_ptr<Expression> parse_logical_or_expression() {}

    std::unique_ptr<StringLit> parse_string_literal() {
        auto t = expect(TokenType::String);
        return new_string_literal(std::move(t));
    }

    std::unique_ptr<StringLit> new_string_literal(std::unique_ptr<Token> t) {
        auto result = StrConv::parse_string(t->lit);
        if (!result.ok()) {
            return nullptr;
        }
        auto ret = std::make_unique<StringLit>();
        ret->base = base_node_from_token(t.get());
        ret->value = result.t();
        return ret;
    }

    std::unique_ptr<Expression> create_bad_expression(std::unique_ptr<Token> tok) {
        std::string ss = "invalid token for primary expression: " + token_to_string(tok->tok);
        return create_bad_expression_with_text(std::move(tok), std::move(ss));
    }

    std::unique_ptr<Expression> create_bad_expression_with_text(std::unique_ptr<Token> tok,
                                                                std::string_view text) {
        auto base = std::make_shared<BaseNode>();
        base->location = source_location(tok->start_pos, tok->end_pos);

        auto expr = std::make_shared<BadExpr>();
        expr->text = text;
        expr->base = base;

        auto ret = std::make_unique<Expression>();
        ret->type = Expression::Type::BadExpr;
        ret->expr = expr;

        return ret;
    }

    std::unique_ptr<Identifier> parse_identifier() {
        auto t = expect_or_skip(TokenType::Ident);
        auto ret = std::make_unique<Identifier>();
        ret->base = base_node_from_token(t.get());
        ret->name = t->lit;
        return ret;
    }

    std::unique_ptr<IntegerLit> parse_int_literal() {
        auto t = expect(TokenType::Int);
        auto ret = std::make_unique<IntegerLit>();
        ret->base = base_node_from_token(t.get());
        if (t->lit.starts_with('0') && t->lit.length() > 1) {
            errs_.emplace_back("invalid integer literal " + t->lit +
                               ": nonzero value cannot start with 0");
            ret->value = 0;
            return ret;
        }

        try {
            int64_t value = std::stol(t->lit);
            ret->value = value;
        } catch (...) {
            errs_.emplace_back("invalid integer literal " + t->lit + ": value out of range");
            ret->value = 0;
        }

        return ret;
    }

    std::tuple<std::unique_ptr<FloatLit>, TokenError> parse_float_literal() {
        auto t = expect(TokenType::Float);
        try {
            long double value = std::stod(t->lit);
            auto ret = std::make_unique<FloatLit>();
            ret->base = base_node_from_token(t.get());
            ret->value = value;
            return {std::move(ret), TokenError()};
        } catch (...) {
            TokenError tok_err;
            tok_err.token = std::move(t);
            return {std::unique_ptr<FloatLit>(), std::move(tok_err)};
        }
    }

    std::unique_ptr<PackageClause>
    parse_package_clause(std::vector<std::shared_ptr<Attribute>>* attributes) {
        return nullptr;
    }

    std::vector<std::shared_ptr<ImportDeclaration>>
    parse_import_list(std::vector<std::shared_ptr<Attribute>>* attributes) {
        return {};
    }

    std::vector<std::shared_ptr<Statement>>
    parse_statement_list(std::vector<std::shared_ptr<Attribute>>* attributes) {
        return {};
    }

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
