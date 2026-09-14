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
// Created: 2026/09/14 13:44

#include <string>

#include "cpp/pl/prism/dialect/dialect.h"
#include "cpp/pl/prism/printer/sql_printer.h"
#include "cpp/pl/prism/syntax/parser.h"
#include "gtest/gtest.h"

namespace pl::prism::printer {
namespace {

// Parses an expression and prints it back with the given dialect.
std::string print_expr(std::string_view sql, const dialect::Dialect& d = dialect::trino()) {
    syntax::Parser parser(sql);
    const syntax::ParseResult result = parser.parse_expression();
    EXPECT_TRUE(result.ok()) << sql;
    return print(result.root, d);
}

// Parses a statement and prints it back with the given dialect.
std::string print_stmt(std::string_view sql, const dialect::Dialect& d = dialect::trino()) {
    syntax::Parser parser(sql);
    const syntax::ParseResult result = parser.parse_statement();
    EXPECT_TRUE(result.ok()) << sql;
    return print(result.root, d);
}

TEST(SqlPrinter, MinimalParens) {
    EXPECT_EQ("a * (b + c)", print_expr("a * (b + c)"));
    EXPECT_EQ("(a + b) * c", print_expr("(a + b) * c"));
    EXPECT_EQ("a * b + c", print_expr("(a * b) + c"));
    EXPECT_EQ("a + b * c", print_expr("a + (b * c)"));
    EXPECT_EQ("a - (b - c)", print_expr("a - (b - c)"));
    EXPECT_EQ("- -9", print_expr("- - 9"));
    EXPECT_EQ("a - b - c", print_expr("a - b - c"));
    EXPECT_EQ("a - (b + c)", print_expr("a - (b + c)"));
    EXPECT_EQ("a || (b || c)", print_expr("a || (b || c)"));
    EXPECT_EQ("a || b || c", print_expr("a || b || c"));
    EXPECT_EQ("a OR b AND c", print_expr("a OR (b AND c)"));
    EXPECT_EQ("(a OR b) AND c", print_expr("(a OR b) AND c"));
    EXPECT_EQ("NOT (a AND b)", print_expr("NOT (a AND b)"));
    EXPECT_EQ("NOT a = b", print_expr("NOT (a = b)"));
    EXPECT_EQ("-(a + b)", print_expr("-(a + b)"));
    EXPECT_EQ("(a = b) IS NULL", print_expr("(a = b) IS NULL"));
}

TEST(SqlPrinter, RowsNormalizeToKeywordForm) {
    EXPECT_EQ("ROW(1, 2)", print_expr("(1, 2)"));
    EXPECT_EQ("ROW()", print_expr("ROW()"));
    EXPECT_EQ("ROW(1)", print_expr("ROW(1)"));
    EXPECT_EQ("ROW(1, (2 + 3) * 4)", print_expr("(1, (2 + 3) * 4)"));
    EXPECT_EQ("VALUES ROW(1, 2), ROW(3, 4)", print_stmt("VALUES (1, 2), (3, 4)"));
}

TEST(SqlPrinter, PartialPredicatesInCaseWhen) {
    EXPECT_EQ("CASE x WHEN > 5 THEN 1 ELSE 0 END", print_expr("CASE x WHEN > 5 THEN 1 ELSE 0 END"));
    EXPECT_EQ("CASE x WHEN BETWEEN 1 AND 4 THEN 1 END",
              print_expr("CASE x WHEN BETWEEN 1 AND 4 THEN 1 END"));
    EXPECT_EQ("CASE x WHEN IS NULL THEN 1 END", print_expr("CASE x WHEN IS NULL THEN 1 END"));
    EXPECT_EQ("CASE x WHEN IN (1, 2) THEN 1 END", print_expr("CASE x WHEN IN (1, 2) THEN 1 END"));
    EXPECT_EQ("CASE x WHEN LIKE 'a%' THEN 1 END", print_expr("CASE x WHEN LIKE 'a%' THEN 1 END"));
    EXPECT_EQ("CASE x WHEN IS TRUE THEN 1 END", print_expr("CASE x WHEN IS TRUE THEN 1 END"));
}

TEST(SqlPrinter, BetweenSymmetry) {
    EXPECT_EQ("a BETWEEN 1 AND 2", print_expr("a BETWEEN ASYMMETRIC 1 AND 2"));
    EXPECT_EQ("a BETWEEN SYMMETRIC 2 AND 1", print_expr("a BETWEEN SYMMETRIC 2 AND 1"));
    EXPECT_EQ("a NOT BETWEEN 1 AND 2", print_expr("a NOT BETWEEN 1 AND 2"));
}

TEST(SqlPrinter, WindowFunctions) {
    EXPECT_EQ("row_number() OVER (PARTITION BY a ORDER BY b DESC NULLS LAST ROWS BETWEEN "
              "UNBOUNDED PRECEDING AND CURRENT ROW)",
              print_expr("row_number() OVER (PARTITION BY a ORDER BY b DESC NULLS LAST "
                         "ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)"));
    EXPECT_EQ("sum(x) OVER (w ORDER BY y RANGE 1 PRECEDING)",
              print_expr("sum(x) OVER (w ORDER BY y RANGE 1 PRECEDING)"));
    EXPECT_EQ("sum(x) FILTER (WHERE x > 0) OVER w",
              print_expr("sum(x) FILTER (WHERE x > 0) OVER w"));
    EXPECT_EQ("array_agg(x ORDER BY y) OVER ()", print_expr("array_agg(x ORDER BY y) OVER ()"));
}

TEST(SqlPrinter, Listagg) {
    EXPECT_EQ("LISTAGG(x) WITHIN GROUP (ORDER BY y)",
              print_expr("LISTAGG(x) WITHIN GROUP (ORDER BY y)"));
    EXPECT_EQ("LISTAGG(DISTINCT x, ',' ON OVERFLOW TRUNCATE '...' WITH COUNT) "
              "WITHIN GROUP (ORDER BY y)",
              print_expr("LISTAGG(DISTINCT x, ',' ON OVERFLOW TRUNCATE '...' WITH COUNT) "
                         "WITHIN GROUP (ORDER BY y)"));
    EXPECT_EQ("LISTAGG(x, ',' ON OVERFLOW ERROR) WITHIN GROUP (ORDER BY y)",
              print_expr("LISTAGG(x, ',' ON OVERFLOW ERROR) WITHIN GROUP (ORDER BY y)"));
}

TEST(SqlPrinter, GroupingSetsAsFunctionCallShape) {
    EXPECT_EQ("SELECT a FROM t GROUP BY GROUPING SETS(ROW(a), ROW())",
              print_stmt("SELECT a FROM t GROUP BY GROUPING SETS ((a), ())"));
    EXPECT_EQ("SELECT a FROM t GROUP BY CUBE(a, b), ROLLUP(c)",
              print_stmt("SELECT a FROM t GROUP BY CUBE(a, b), ROLLUP(c)"));
    EXPECT_EQ("SELECT a FROM t GROUP BY ROW()", print_stmt("SELECT a FROM t GROUP BY ()"));
    EXPECT_EQ("SELECT a FROM t GROUP BY AUTO", print_stmt("SELECT a FROM t GROUP BY AUTO"));
    EXPECT_EQ("SELECT a FROM t GROUP BY DISTINCT a",
              print_stmt("SELECT a FROM t GROUP BY DISTINCT a"));
}

TEST(SqlPrinter, DefaultQuantifiersDropped) {
    // SELECT ALL, GROUP BY ALL and LIMIT ALL leave no trace in the AST.
    EXPECT_EQ("SELECT a FROM t", print_stmt("SELECT ALL a FROM t"));
    EXPECT_EQ("SELECT a FROM t GROUP BY a", print_stmt("SELECT a FROM t GROUP BY ALL a"));
    EXPECT_EQ("SELECT * FROM t", print_stmt("SELECT * FROM t LIMIT ALL"));
    EXPECT_EQ("SELECT NULL FROM t", print_stmt("SELECT NULL FROM t"));
}

TEST(SqlPrinter, RelationParens) {
    // An aliased join keeps its parens.
    EXPECT_EQ("SELECT * FROM (t1 INNER JOIN t2 ON t1.a = t2.a) AS j",
              print_stmt("SELECT * FROM (t1 JOIN t2 ON t1.a = t2.a) j"));
    // A right-side join keeps its parens; left chains stay flat.
    EXPECT_EQ("SELECT * FROM a INNER JOIN (b INNER JOIN c ON b.x = c.x) ON a.x = c.x",
              print_stmt("SELECT * FROM a JOIN (b JOIN c ON b.x = c.x) ON a.x = c.x"));
    EXPECT_EQ("SELECT * FROM a INNER JOIN b ON a.x = b.x INNER JOIN c ON b.y = c.y",
              print_stmt("SELECT * FROM a JOIN b ON a.x = b.x JOIN c ON b.y = c.y"));
    // Aliases always print with AS.
    EXPECT_EQ("SELECT * FROM t AS u (x, y)", print_stmt("SELECT * FROM t u(x, y)"));
    EXPECT_EQ("SELECT * FROM UNNEST(a) WITH ORDINALITY AS u (x, ord)",
              print_stmt("SELECT * FROM UNNEST(a) WITH ORDINALITY u(x, ord)"));
}

TEST(SqlPrinter, SetOperationParens) {
    EXPECT_EQ("(SELECT 1 UNION SELECT 2) INTERSECT SELECT 3",
              print_stmt("(SELECT 1 UNION SELECT 2) INTERSECT SELECT 3"));
    EXPECT_EQ("SELECT 1 UNION SELECT 2 INTERSECT SELECT 3",
              print_stmt("SELECT 1 UNION SELECT 2 INTERSECT SELECT 3"));
    EXPECT_EQ("SELECT 1 UNION (SELECT 2 UNION SELECT 3)",
              print_stmt("SELECT 1 UNION (SELECT 2 UNION SELECT 3)"));
    EXPECT_EQ("SELECT 1 UNION ALL SELECT 2 EXCEPT SELECT 3",
              print_stmt("SELECT 1 UNION ALL SELECT 2 EXCEPT SELECT 3"));
    // UNION DISTINCT is the default and leaves no trace in the AST.
    EXPECT_EQ("SELECT 1 UNION SELECT 2", print_stmt("SELECT 1 UNION DISTINCT SELECT 2"));
    EXPECT_EQ("VALUES ROW(1) UNION SELECT 2", print_stmt("VALUES (1) UNION SELECT 2"));
    EXPECT_EQ("TABLE t UNION SELECT a FROM t", print_stmt("TABLE t UNION SELECT a FROM t"));
}

TEST(SqlPrinter, QuerySuffixes) {
    EXPECT_EQ("SELECT * FROM t ORDER BY a ASC NULLS FIRST OFFSET 5 ROWS LIMIT 10",
              print_stmt("SELECT * FROM t ORDER BY a ASC NULLS FIRST OFFSET 5 LIMIT 10"));
    EXPECT_EQ("SELECT * FROM t FETCH FIRST 5 ROWS WITH TIES",
              print_stmt("SELECT * FROM t FETCH FIRST 5 ROWS WITH TIES"));
    EXPECT_EQ("SELECT * FROM t FETCH FIRST ROWS WITH TIES",
              print_stmt("SELECT * FROM t FETCH FIRST ROWS WITH TIES"));
    EXPECT_EQ("SELECT * FROM t ORDER BY a LIMIT 10",
              print_stmt("SELECT * FROM t ORDER BY a LIMIT 10"));
}

TEST(SqlPrinter, WithAndExplain) {
    EXPECT_EQ("WITH RECURSIVE t (x) AS (SELECT 1) SELECT x FROM t",
              print_stmt("WITH RECURSIVE t(x) AS (SELECT 1) SELECT x FROM t"));
    EXPECT_EQ("EXPLAIN ANALYZE SELECT 1", print_stmt("EXPLAIN ANALYZE SELECT 1"));
    EXPECT_EQ("EXPLAIN (FORMAT JSON) SELECT 1", print_stmt("EXPLAIN (FORMAT JSON) SELECT 1"));
}

TEST(SqlPrinter, SparkIdentifierQuoting) {
    EXPECT_EQ("SELECT `a b` FROM `my table`",
              print_stmt("SELECT \"a b\" FROM \"my table\"", dialect::spark()));
    EXPECT_EQ("SELECT `a\"b` FROM t", print_stmt("SELECT \"a\"\"b\" FROM t", dialect::spark()));
    // Trino keeps the raw quoted text.
    EXPECT_EQ("SELECT \"a b\" FROM t", print_stmt("SELECT \"a b\" FROM t"));
}

TEST(SqlPrinter, SparkIlikeRewrite) {
    EXPECT_EQ("SELECT LOWER(a) LIKE LOWER('x%') FROM t",
              print_stmt("SELECT a ILIKE 'x%' FROM t", dialect::spark()));
    EXPECT_EQ("SELECT * FROM t WHERE LOWER(a) NOT LIKE LOWER('x%') ESCAPE '!'",
              print_stmt("SELECT * FROM t WHERE a NOT ILIKE 'x%' ESCAPE '!'", dialect::spark()));
    EXPECT_EQ("SELECT a ILIKE 'x%' FROM t", print_stmt("SELECT a ILIKE 'x%' FROM t"));
}

TEST(SqlPrinter, SparkTryCastFallback) {
    EXPECT_EQ("SELECT CAST(x AS DECIMAL(10, 2)) FROM t",
              print_stmt("SELECT TRY_CAST(x AS DECIMAL(10,2)) FROM t", dialect::spark()));
    EXPECT_EQ("SELECT TRY_CAST(x AS INT) FROM t", print_stmt("SELECT TRY_CAST(x AS INT) FROM t"));
}

} // namespace
} // namespace pl::prism::printer
