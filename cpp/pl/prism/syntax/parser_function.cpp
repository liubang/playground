// Copyright (c) 2026 The Authors. All rights reserved.
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
// Created: 2026/09/14 10:16

// Function calls including aggregate decorations (DISTINCT, wildcard,
// aggregate-internal ORDER BY, FILTER) and window specifications (OVER clause,
// named window references and inheritance, frames). Window specifications are
// shared with the WINDOW clause parsed in parser_query.cpp.

#include <utility>

#include "cpp/pl/prism/syntax/parser.h"

namespace pl::prism::syntax {

Expression* Parser::parse_listagg(SourceLocation loc) {
    advance(); // LISTAGG
    expect(TokenType::kLParen, "'(' after LISTAGG");
    const bool distinct = match(TokenType::kKwDistinct);
    Expression* value = parse_expr();
    Expression* separator = nullptr;
    OverflowBehavior overflow = OverflowBehavior::kUnspecified;
    std::string_view overflow_filler;
    OverflowCount overflow_count = OverflowCount::kUnspecified;
    if (match(TokenType::kComma)) {
        separator = parse_expr();
        if (match(TokenType::kKwOn)) {
            if (!match_soft("overflow")) {
                fail(cur(), "expected OVERFLOW after ON");
            }
            if (match_soft("error")) {
                overflow = OverflowBehavior::kError;
            } else if (match_soft("truncate")) {
                overflow = OverflowBehavior::kTruncate;
                if (at(TokenType::kString)) {
                    overflow_filler = cur_text();
                    advance();
                }
                if (match(TokenType::kKwWith)) {
                    if (!match_soft("count")) {
                        fail(cur(), "expected COUNT after WITH");
                    }
                    overflow_count = OverflowCount::kWith;
                } else if (match_soft("without")) {
                    if (!match_soft("count")) {
                        fail(cur(), "expected COUNT after WITHOUT");
                    }
                    overflow_count = OverflowCount::kWithout;
                }
            } else {
                fail(cur(), "expected ERROR or TRUNCATE after ON OVERFLOW");
            }
        }
    }
    expect(TokenType::kRParen, "')' after LISTAGG arguments");
    if (!match_soft("within")) {
        fail(cur(), "expected WITHIN GROUP after LISTAGG");
    }
    expect(TokenType::kKwGroup, "GROUP after WITHIN");
    expect(TokenType::kLParen, "'(' after WITHIN GROUP");
    expect(TokenType::kKwOrder, "ORDER in WITHIN GROUP");
    expect(TokenType::kKwBy, "BY after ORDER");
    std::vector<SortItem*> order_by = parse_sort_list();
    expect(TokenType::kRParen, "')' after WITHIN GROUP");
    return make<ListaggExpression>(loc,
                                   distinct,
                                   value,
                                   separator,
                                   overflow,
                                   overflow_filler,
                                   overflow_count,
                                   make_list(order_by));
}

Expression* Parser::parse_function_call(SourceLocation loc, AstList<NamePart> name) {
    expect(TokenType::kLParen, "'(' after function name");
    bool distinct = false;
    bool wildcard = false;
    std::vector<Expression*> args;
    std::vector<SortItem*> order_by;
    if (match(TokenType::kStar)) {
        wildcard = true;
        expect(TokenType::kRParen, "')' after '*'");
    } else if (match(TokenType::kRParen)) {
        // no arguments
    } else {
        distinct = match(TokenType::kKwDistinct);
        args.push_back(parse_expr());
        while (match(TokenType::kComma)) {
            args.push_back(parse_expr());
        }
        // Aggregate-internal ORDER BY, e.g. array_agg(x ORDER BY y).
        if (match(TokenType::kKwOrder)) {
            expect(TokenType::kKwBy, "BY after ORDER");
            order_by = parse_sort_list();
        }
        expect(TokenType::kRParen, "')' after function arguments");
    }
    return finish_function_call(
        loc, name, distinct, wildcard, std::move(args), std::move(order_by));
}

Expression* Parser::finish_function_call(SourceLocation loc,
                                         AstList<NamePart> name,
                                         bool distinct,
                                         bool wildcard,
                                         std::vector<Expression*> args,
                                         std::vector<SortItem*> order_by) {
    // FILTER (WHERE ...) on aggregates.
    Expression* filter = nullptr;
    if (at_soft("filter")) {
        advance();
        expect(TokenType::kLParen, "'(' after FILTER");
        expect(TokenType::kKwWhere, "WHERE in FILTER");
        filter = parse_expr();
        expect(TokenType::kRParen, "')' after FILTER");
    }

    // OVER (...) or OVER window_name.
    Window* window = nullptr;
    NamePart window_ref{};
    bool has_window_ref = false;
    if (at_soft("over")) {
        advance();
        if (at(TokenType::kLParen)) {
            window = parse_window();
        } else if (at_name()) {
            window_ref = parse_name_part();
            has_window_ref = true;
        } else {
            fail(cur(), "expected window specification or window name after OVER");
        }
    }
    return make<FunctionCall>(loc,
                              name,
                              distinct,
                              wildcard,
                              make_list(args),
                              filter,
                              make_list(order_by),
                              window,
                              window_ref,
                              has_window_ref);
}

Window* Parser::parse_window() {
    const SourceLocation loc = loc_of(cur());
    expect(TokenType::kLParen, "'(' after OVER");
    // A window specification may derive from a named window defined in the
    // WINDOW clause: (someWindow ORDER BY b).
    NamePart existing_window{};
    bool has_existing_window = false;
    if (at_name() && !at_soft("partition") && !at_soft("rows") && !at_soft("range") &&
        !at_soft("groups")) {
        existing_window = parse_name_part();
        has_existing_window = true;
    }
    std::vector<Expression*> partition_by;
    if (match_soft("partition")) {
        expect(TokenType::kKwBy, "BY after PARTITION");
        partition_by.push_back(parse_expr());
        while (match(TokenType::kComma)) {
            partition_by.push_back(parse_expr());
        }
    }
    std::vector<SortItem*> order_by;
    if (match(TokenType::kKwOrder)) {
        expect(TokenType::kKwBy, "BY after ORDER");
        order_by = parse_sort_list();
    }
    WindowFrame* frame = nullptr;
    if (at_soft("rows") || at_soft("range") || at_soft("groups")) {
        frame = parse_frame();
    }
    expect(TokenType::kRParen, "')' after window specification");
    return make<Window>(loc,
                        existing_window,
                        has_existing_window,
                        make_list(partition_by),
                        make_list(order_by),
                        frame);
}

WindowFrame* Parser::parse_frame() {
    const SourceLocation loc = loc_of(cur());
    FrameType frame_type = FrameType::kRows;
    if (match_soft("range")) {
        frame_type = FrameType::kRange;
    } else if (match_soft("groups")) {
        frame_type = FrameType::kGroups;
    } else {
        advance(); // "rows"
    }
    if (match(TokenType::kKwBetween)) {
        const FrameBound start = parse_frame_bound();
        expect(TokenType::kKwAnd, "AND in window frame");
        const FrameBound end = parse_frame_bound();
        return make<WindowFrame>(loc, frame_type, start, end, true);
    }
    const FrameBound start = parse_frame_bound();
    return make<WindowFrame>(loc, frame_type, start, FrameBound{}, false);
}

FrameBound Parser::parse_frame_bound() {
    if (match_soft("unbounded")) {
        if (match_soft("preceding")) {
            return FrameBound{FrameBoundType::kUnboundedPreceding, nullptr};
        }
        if (!match_soft("following")) {
            fail(cur(), "expected PRECEDING or FOLLOWING after UNBOUNDED");
        }
        return FrameBound{FrameBoundType::kUnboundedFollowing, nullptr};
    }
    if (at(TokenType::kKwCurrent) || at_soft("current")) {
        advance();
        if (!match_soft("row")) {
            fail(cur(), "expected ROW after CURRENT");
        }
        return FrameBound{FrameBoundType::kCurrentRow, nullptr};
    }
    Expression* value = parse_expr();
    if (match_soft("preceding")) {
        return FrameBound{FrameBoundType::kPreceding, value};
    }
    if (!match_soft("following")) {
        fail(cur(), "expected PRECEDING or FOLLOWING in window frame bound");
    }
    return FrameBound{FrameBoundType::kFollowing, value};
}

} // namespace pl::prism::syntax
