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
// Created: 2026/09/14 16:10

// prism — command line frontend for the Prism SQL parser.
//
//   prism parse   [--expr] [sql...]              parse and report errors
//   prism ast     [--expr] [sql...]              print the AST s-expression
//   prism print   [--expr] [--dialect D] [sql...]  print canonical/rewritten SQL
//   prism plan    [sql...]                       print the logical plan s-expression
//   prism tokens  [sql...]                       print the lexer token stream
//
// SQL is taken from the remaining arguments (joined with spaces) or, when
// absent, from standard input. D is trino (default) or spark.
//
// Exit codes: 0 success, 1 usage error, 2 parse/plan failure.

#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/pl/prism/dialect/dialect.h"
#include "cpp/pl/prism/plan/ast_to_plan.h"
#include "cpp/pl/prism/plan/plan_dump.h"
#include "cpp/pl/prism/printer/sql_printer.h"
#include "cpp/pl/prism/syntax/ast_dump.h"
#include "cpp/pl/prism/syntax/lexer.h"
#include "cpp/pl/prism/syntax/parser.h"

namespace {

namespace syn = pl::prism::syntax;

constexpr int kExitUsage = 1;
constexpr int kExitFailure = 2;

const char* usage_text = R"(prism — command line frontend for the Prism SQL parser

Usage:
  prism parse   [--expr] [sql...]                parse and report errors
  prism ast     [--expr] [sql...]                print the AST s-expression
  prism print   [--expr] [--dialect D] [sql...]  print canonical/rewritten SQL
  prism plan    [sql...]                         print the logical plan s-expression
  prism tokens  [sql...]                         print the lexer token stream

SQL is read from the remaining arguments (joined with spaces) or, when
absent, from standard input. D is trino (default) or spark.

Exit codes: 0 success, 1 usage error, 2 parse/plan failure.
)";

const char* token_type_name(syn::TokenType type) {
    switch (type) {
        case syn::TokenType::kEof:
            return "EOF";
        case syn::TokenType::kIllegal:
            return "ILLEGAL";
        case syn::TokenType::kIdentifier:
            return "IDENT";
        case syn::TokenType::kQuotedIdentifier:
            return "QIDENT";
        case syn::TokenType::kString:
            return "STRING";
        case syn::TokenType::kUnicodeString:
            return "USTRING";
        case syn::TokenType::kBinaryString:
            return "BSTRING";
        case syn::TokenType::kNumber:
            return "NUMBER";
        case syn::TokenType::kLParen:
            return "(";
        case syn::TokenType::kRParen:
            return ")";
        case syn::TokenType::kLBracket:
            return "[";
        case syn::TokenType::kRBracket:
            return "]";
        case syn::TokenType::kComma:
            return ",";
        case syn::TokenType::kSemicolon:
            return ";";
        case syn::TokenType::kDot:
            return ".";
        case syn::TokenType::kColon:
            return ":";
        case syn::TokenType::kDoubleColon:
            return "::";
        case syn::TokenType::kPlus:
            return "+";
        case syn::TokenType::kMinus:
            return "-";
        case syn::TokenType::kStar:
            return "*";
        case syn::TokenType::kSlash:
            return "/";
        case syn::TokenType::kPercent:
            return "%";
        case syn::TokenType::kEq:
            return "=";
        case syn::TokenType::kNeq:
            return "<>";
        case syn::TokenType::kLt:
            return "<";
        case syn::TokenType::kGt:
            return ">";
        case syn::TokenType::kLte:
            return "<=";
        case syn::TokenType::kGte:
            return ">=";
        case syn::TokenType::kConcat:
            return "||";
        case syn::TokenType::kArrow:
            return "->";
        case syn::TokenType::kFatArrow:
            return "=>";
        case syn::TokenType::kQuestion:
            return "?";
        case syn::TokenType::kKwAlter:
            return "ALTER";
        case syn::TokenType::kKwAnalyze:
            return "ANALYZE";
        case syn::TokenType::kKwAnd:
            return "AND";
        case syn::TokenType::kKwArray:
            return "ARRAY";
        case syn::TokenType::kKwAs:
            return "AS";
        case syn::TokenType::kKwAt:
            return "AT";
        case syn::TokenType::kKwBetween:
            return "BETWEEN";
        case syn::TokenType::kKwBy:
            return "BY";
        case syn::TokenType::kKwCase:
            return "CASE";
        case syn::TokenType::kKwCast:
            return "CAST";
        case syn::TokenType::kKwConstraint:
            return "CONSTRAINT";
        case syn::TokenType::kKwCreate:
            return "CREATE";
        case syn::TokenType::kKwCross:
            return "CROSS";
        case syn::TokenType::kKwCube:
            return "CUBE";
        case syn::TokenType::kKwCurrent:
            return "CURRENT";
        case syn::TokenType::kKwCurrentCatalog:
            return "CURRENT_CATALOG";
        case syn::TokenType::kKwCurrentDate:
            return "CURRENT_DATE";
        case syn::TokenType::kKwCurrentPath:
            return "CURRENT_PATH";
        case syn::TokenType::kKwCurrentRole:
            return "CURRENT_ROLE";
        case syn::TokenType::kKwCurrentSchema:
            return "CURRENT_SCHEMA";
        case syn::TokenType::kKwCurrentTime:
            return "CURRENT_TIME";
        case syn::TokenType::kKwCurrentTimestamp:
            return "CURRENT_TIMESTAMP";
        case syn::TokenType::kKwCurrentUser:
            return "CURRENT_USER";
        case syn::TokenType::kKwDeallocate:
            return "DEALLOCATE";
        case syn::TokenType::kKwDelete:
            return "DELETE";
        case syn::TokenType::kKwDescribe:
            return "DESCRIBE";
        case syn::TokenType::kKwDistinct:
            return "DISTINCT";
        case syn::TokenType::kKwDrop:
            return "DROP";
        case syn::TokenType::kKwElse:
            return "ELSE";
        case syn::TokenType::kKwEnd:
            return "END";
        case syn::TokenType::kKwEscape:
            return "ESCAPE";
        case syn::TokenType::kKwExcept:
            return "EXCEPT";
        case syn::TokenType::kKwExecute:
            return "EXECUTE";
        case syn::TokenType::kKwExists:
            return "EXISTS";
        case syn::TokenType::kKwExplain:
            return "EXPLAIN";
        case syn::TokenType::kKwExtract:
            return "EXTRACT";
        case syn::TokenType::kKwFalse:
            return "FALSE";
        case syn::TokenType::kKwFor:
            return "FOR";
        case syn::TokenType::kKwFrom:
            return "FROM";
        case syn::TokenType::kKwFull:
            return "FULL";
        case syn::TokenType::kKwGroup:
            return "GROUP";
        case syn::TokenType::kKwGrouping:
            return "GROUPING";
        case syn::TokenType::kKwHaving:
            return "HAVING";
        case syn::TokenType::kKwIn:
            return "IN";
        case syn::TokenType::kKwInner:
            return "INNER";
        case syn::TokenType::kKwInsert:
            return "INSERT";
        case syn::TokenType::kKwIntersect:
            return "INTERSECT";
        case syn::TokenType::kKwInterval:
            return "INTERVAL";
        case syn::TokenType::kKwInto:
            return "INTO";
        case syn::TokenType::kKwIs:
            return "IS";
        case syn::TokenType::kKwJoin:
            return "JOIN";
        case syn::TokenType::kKwJsonArray:
            return "JSON_ARRAY";
        case syn::TokenType::kKwJsonExists:
            return "JSON_EXISTS";
        case syn::TokenType::kKwJsonObject:
            return "JSON_OBJECT";
        case syn::TokenType::kKwJsonQuery:
            return "JSON_QUERY";
        case syn::TokenType::kKwJsonTable:
            return "JSON_TABLE";
        case syn::TokenType::kKwJsonValue:
            return "JSON_VALUE";
        case syn::TokenType::kKwLeft:
            return "LEFT";
        case syn::TokenType::kKwLike:
            return "LIKE";
        case syn::TokenType::kKwLimit:
            return "LIMIT";
        case syn::TokenType::kKwListagg:
            return "LISTAGG";
        case syn::TokenType::kKwLocaltime:
            return "LOCALTIME";
        case syn::TokenType::kKwLocaltimestamp:
            return "LOCALTIMESTAMP";
        case syn::TokenType::kKwNatural:
            return "NATURAL";
        case syn::TokenType::kKwNormalize:
            return "NORMALIZE";
        case syn::TokenType::kKwNot:
            return "NOT";
        case syn::TokenType::kKwNull:
            return "NULL";
        case syn::TokenType::kKwOn:
            return "ON";
        case syn::TokenType::kKwOr:
            return "OR";
        case syn::TokenType::kKwOrder:
            return "ORDER";
        case syn::TokenType::kKwOuter:
            return "OUTER";
        case syn::TokenType::kKwPrepare:
            return "PREPARE";
        case syn::TokenType::kKwRecursive:
            return "RECURSIVE";
        case syn::TokenType::kKwRight:
            return "RIGHT";
        case syn::TokenType::kKwRollup:
            return "ROLLUP";
        case syn::TokenType::kKwSelect:
            return "SELECT";
        case syn::TokenType::kKwTable:
            return "TABLE";
        case syn::TokenType::kKwThen:
            return "THEN";
        case syn::TokenType::kKwTo:
            return "TO";
        case syn::TokenType::kKwTrue:
            return "TRUE";
        case syn::TokenType::kKwUescape:
            return "UESCAPE";
        case syn::TokenType::kKwUnion:
            return "UNION";
        case syn::TokenType::kKwUnnest:
            return "UNNEST";
        case syn::TokenType::kKwUsing:
            return "USING";
        case syn::TokenType::kKwValues:
            return "VALUES";
        case syn::TokenType::kKwWhen:
            return "WHEN";
        case syn::TokenType::kKwWhere:
            return "WHERE";
        case syn::TokenType::kKwWith:
            return "WITH";
    }
    return "?";
}

struct Options {
    bool expr = false;
    const pl::prism::dialect::Dialect* dialect = &pl::prism::dialect::trino();
    std::string sql;
};

std::string read_stdin() {
    std::ostringstream buffer;
    buffer << std::cin.rdbuf();
    return buffer.str();
}

void report_parse_errors(const syn::ParseResult& result) {
    for (const syn::ParseError& error : result.errors) {
        std::cerr << error.line << ':' << error.column << ": " << error.message << '\n';
    }
}

int cmd_parse(const Options& opts) {
    syn::Parser parser(opts.sql);
    const syn::ParseResult result =
        opts.expr ? parser.parse_expression() : parser.parse_statement();
    if (!result.ok()) {
        report_parse_errors(result);
        return kExitFailure;
    }
    std::cout << "OK\n";
    return 0;
}

int cmd_ast(const Options& opts) {
    syn::Parser parser(opts.sql);
    const syn::ParseResult result =
        opts.expr ? parser.parse_expression() : parser.parse_statement();
    if (!result.ok()) {
        report_parse_errors(result);
        return kExitFailure;
    }
    std::cout << syn::dump(result.root) << '\n';
    return 0;
}

int cmd_print(const Options& opts) {
    syn::Parser parser(opts.sql);
    const syn::ParseResult result =
        opts.expr ? parser.parse_expression() : parser.parse_statement();
    if (!result.ok()) {
        report_parse_errors(result);
        return kExitFailure;
    }
    std::cout << pl::prism::printer::print(result.root, *opts.dialect) << '\n';
    return 0;
}

int cmd_plan(const Options& opts) {
    syn::Parser parser(opts.sql);
    const syn::ParseResult parsed = parser.parse_statement();
    if (!parsed.ok()) {
        report_parse_errors(parsed);
        return kExitFailure;
    }
    pl::prism::plan::AstToPlan planner;
    const pl::prism::plan::PlanResult plan = planner.translate(parsed.root);
    if (!plan.ok()) {
        for (const pl::prism::plan::PlanError& error : plan.errors) {
            std::cerr << error.location.line << ':' << error.location.column << ": "
                      << error.message << '\n';
        }
        return kExitFailure;
    }
    std::cout << pl::prism::plan::dump(plan.root) << '\n';
    return 0;
}

int cmd_tokens(const Options& opts) {
    syn::Lexer lexer(opts.sql);
    for (;;) {
        const syn::Token token = lexer.next_token();
        if (token.type == syn::TokenType::kEof) {
            break;
        }
        std::cout << token.line << ':' << token.column << '\t' << token_type_name(token.type)
                  << '\t' << token.text(opts.sql) << '\n';
    }
    if (!lexer.errors().empty()) {
        for (const syn::LexError& error : lexer.errors()) {
            std::cerr << error.line << ':' << error.column << ": " << error.message << '\n';
        }
        return kExitFailure;
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << usage_text;
        return kExitUsage;
    }
    const std::string_view command = argv[1];
    if (command == "-h" || command == "--help" || command == "help") {
        std::cout << usage_text;
        return 0;
    }

    Options opts;
    std::vector<std::string_view> sql_parts;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "-e" || arg == "--expr") {
            opts.expr = true;
        } else if (arg == "-d" || arg == "--dialect") {
            if (++i >= argc) {
                std::cerr << "missing value for " << arg << "\n\n" << usage_text;
                return kExitUsage;
            }
            const std::string_view name = argv[i];
            if (name == "spark") {
                opts.dialect = &pl::prism::dialect::spark();
            } else if (name != "trino") {
                std::cerr << "unknown dialect: " << name << "\n\n" << usage_text;
                return kExitUsage;
            }
        } else if (arg.rfind("--dialect=", 0) == 0) {
            const std::string_view name = arg.substr(10);
            if (name == "spark") {
                opts.dialect = &pl::prism::dialect::spark();
            } else if (name != "trino") {
                std::cerr << "unknown dialect: " << name << "\n\n" << usage_text;
                return kExitUsage;
            }
        } else if (!arg.empty() && arg[0] == '-' && arg != "-") {
            std::cerr << "unknown option: " << arg << "\n\n" << usage_text;
            return kExitUsage;
        } else {
            sql_parts.push_back(arg);
        }
    }
    if (sql_parts.empty()) {
        opts.sql = read_stdin();
    } else {
        for (size_t i = 0; i < sql_parts.size(); ++i) {
            if (i > 0) {
                opts.sql.push_back(' ');
            }
            opts.sql.append(sql_parts[i]);
        }
    }

    if (command == "parse") {
        return cmd_parse(opts);
    }
    if (command == "ast") {
        return cmd_ast(opts);
    }
    if (command == "print") {
        return cmd_print(opts);
    }
    if (command == "plan") {
        return cmd_plan(opts);
    }
    if (command == "tokens") {
        return cmd_tokens(opts);
    }
    std::cerr << "unknown command: " << command << "\n\n" << usage_text;
    return kExitUsage;
}
