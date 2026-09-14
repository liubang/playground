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
// Created: 2026/09/13 13:24

#include <string>
#include <string_view>

#include "cpp/pl/prism/syntax/ast_dump.h"
#include "cpp/pl/prism/syntax/parser.h"
#include "gtest/gtest.h"

namespace pl::prism::syntax {
namespace {

std::string dump_statement(std::string_view sql) {
    Parser parser(sql);
    ParseResult result = parser.parse_statement();
    if (!result.ok()) {
        ADD_FAILURE() << "parse failed for: " << sql << "\n  error: " << result.errors[0].message
                      << " at line " << result.errors[0].line << ", column "
                      << result.errors[0].column;
        return {};
    }
    return dump(result.root);
}

std::string dump_expression(std::string_view sql) {
    Parser parser(sql);
    ParseResult result = parser.parse_expression();
    if (!result.ok()) {
        ADD_FAILURE() << "parse failed for: " << sql << "\n  error: " << result.errors[0].message
                      << " at line " << result.errors[0].line << ", column "
                      << result.errors[0].column;
        return {};
    }
    return dump(result.root);
}

bool has_error(std::string_view sql) {
    Parser parser(sql);
    return !parser.parse_statement().ok();
}

TEST(ParserTest, SelectLiteral) {
    EXPECT_EQ("(query (spec (select (col 1))))", dump_statement("SELECT 1"));
}

TEST(ParserTest, SelectStarFromTable) {
    EXPECT_EQ("(query (spec (select *) (from (table t))))", dump_statement("SELECT * FROM t"));
}

TEST(ParserTest, QualifiedNames) {
    EXPECT_EQ("(query (spec (select (col a.b.c)) (from (table s.t))))",
              dump_statement("SELECT a.b.c FROM s.t"));
}

TEST(ParserTest, PrefixedStar) {
    EXPECT_EQ("(query (spec (select t.*) (from (table t))))", dump_statement("SELECT t.* FROM t"));
}

TEST(ParserTest, SelectAliases) {
    EXPECT_EQ("(query (spec (select (col a) (col b c) (col d e))))",
              dump_statement("SELECT a, b AS c, d e"));
}

TEST(ParserTest, ArithmeticPrecedence) {
    EXPECT_EQ("(+ 1 (* 2 3))", dump_expression("1 + 2 * 3"));
    EXPECT_EQ("(* (+ 1 2) 3)", dump_expression("(1 + 2) * 3"));
}

TEST(ParserTest, ConcatBindsLooserThanAdditive) {
    EXPECT_EQ("(|| (+ a b) (- c d))", dump_expression("a + b || c - d"));
}

TEST(ParserTest, UnarySign) {
    EXPECT_EQ("(+ (neg x) (pos y))", dump_expression("-x + +y"));
}

TEST(ParserTest, LogicalPrecedence) {
    EXPECT_EQ("(or (and (= a 1) (<> b 2)) (not (isnull c)))",
              dump_expression("a = 1 AND b <> 2 OR NOT c IS NULL"));
}

TEST(ParserTest, BetweenAndAnd) {
    EXPECT_EQ("(and (between x 1 2) (= y 3))", dump_expression("x BETWEEN 1 AND 2 AND y = 3"));
    EXPECT_EQ("(notbetween x 1 2)", dump_expression("x NOT BETWEEN 1 AND 2"));
}

TEST(ParserTest, InPredicates) {
    EXPECT_EQ("(in x (list 1 2 3))", dump_expression("x IN (1, 2, 3)"));
    EXPECT_EQ("(notin x (subquery (query (spec (select (col y)) (from (table t))))))",
              dump_expression("x NOT IN (SELECT y FROM t)"));
}

TEST(ParserTest, LikePredicates) {
    EXPECT_EQ("(like a 'x%' (esc '!'))", dump_expression("a LIKE 'x%' ESCAPE '!'"));
    EXPECT_EQ("(notilike a 'x%')", dump_expression("a NOT ILIKE 'x%'"));
}

TEST(ParserTest, DistinctFrom) {
    EXPECT_EQ("(isdistinct a b)", dump_expression("a IS DISTINCT FROM b"));
    EXPECT_EQ("(isnotdistinct a b)", dump_expression("a IS NOT DISTINCT FROM b"));
}

TEST(ParserTest, CaseExpressions) {
    EXPECT_EQ("(case x (when 1 'a') (when 2 'b') (else 'c'))",
              dump_expression("CASE x WHEN 1 THEN 'a' WHEN 2 THEN 'b' ELSE 'c' END"));
    EXPECT_EQ("(case (when (> x 1) 2))", dump_expression("CASE WHEN x > 1 THEN 2 END"));
}

TEST(ParserTest, Casts) {
    EXPECT_EQ("(cast x DECIMAL(10,2))", dump_expression("CAST(x AS DECIMAL(10,2))"));
    EXPECT_EQ("(trycast x VARCHAR)", dump_expression("TRY_CAST(x AS VARCHAR)"));
    EXPECT_EQ("(cast x ARRAY(BIGINT))", dump_expression("CAST(x AS ARRAY(BIGINT))"));
    EXPECT_EQ("(cast x ROW(a INT, b VARCHAR(3)))",
              dump_expression("CAST(x AS ROW(a INT, b VARCHAR(3)))"));
    EXPECT_EQ("(cast x DOUBLE PRECISION)", dump_expression("CAST(x AS DOUBLE PRECISION)"));
    EXPECT_EQ("(cast x TIMESTAMP WITH TIME ZONE)",
              dump_expression("CAST(x AS TIMESTAMP WITH TIME ZONE)"));
}

TEST(ParserTest, FunctionCalls) {
    EXPECT_EQ("(call count *)", dump_expression("count(*)"));
    EXPECT_EQ("(call count distinct x)", dump_expression("count(distinct x)"));
    EXPECT_EQ("(call sys.fn a b)", dump_expression("sys.fn(a, b)"));
    EXPECT_EQ("(call now)", dump_expression("now()"));
}

TEST(ParserTest, WindowFunction) {
    EXPECT_EQ("(call sum x (over (part k) (order (item v desc nulls last)) "
              "(frame ROWS (start (preceding 1)) (end (current row)))))",
              dump_expression("sum(x) OVER (PARTITION BY k ORDER BY v DESC NULLS LAST "
                              "ROWS BETWEEN 1 PRECEDING AND CURRENT ROW)"));
    EXPECT_EQ("(call rank (over (order (item x)) "
              "(frame RANGE (start (unbounded preceding)) (end (unbounded following)))))",
              dump_expression("rank() OVER (ORDER BY x RANGE BETWEEN UNBOUNDED PRECEDING "
                              "AND UNBOUNDED FOLLOWING)"));
    EXPECT_EQ("(call sum x (over (frame ROWS (start (preceding 2)))))",
              dump_expression("sum(x) OVER (ROWS 2 PRECEDING)"));
    EXPECT_EQ("(call row_number (over))", dump_expression("row_number() OVER ()"));
}

TEST(ParserTest, SubscriptAndDereference) {
    EXPECT_EQ("(subscript arr 1)", dump_expression("arr[1]"));
    EXPECT_EQ("(subscript (subscript arr 1) 2)", dump_expression("arr[1][2]"));
    EXPECT_EQ("(subscript a 1).b", dump_expression("a[1].b"));
}

TEST(ParserTest, Lambda) {
    EXPECT_EQ("(lambda (x) (+ x 1))", dump_expression("x -> x + 1"));
    EXPECT_EQ("(lambda (a b) (+ a b))", dump_expression("(a, b) -> a + b"));
    EXPECT_EQ("(call map_filter m (lambda (k v) (> v 0)))",
              dump_expression("map_filter(m, (k, v) -> v > 0)"));
}

TEST(ParserTest, RowAndArray) {
    EXPECT_EQ("(row 1 'a' true)", dump_expression("(1, 'a', true)"));
    EXPECT_EQ("(row 1 2)", dump_expression("ROW(1, 2)"));
    EXPECT_EQ("(array 1 2)", dump_expression("ARRAY[1, 2]"));
    EXPECT_EQ("(array)", dump_expression("ARRAY[]"));
}

TEST(ParserTest, Literals) {
    EXPECT_EQ("null", dump_expression("NULL"));
    EXPECT_EQ("true", dump_expression("TRUE"));
    EXPECT_EQ("(interval '1' DAY)", dump_expression("INTERVAL '1' DAY"));
    EXPECT_EQ("(interval -'1-2' YEAR TO MONTH)", dump_expression("INTERVAL -'1-2' YEAR TO MONTH"));
    EXPECT_EQ("DATE '2024-01-01'", dump_expression("DATE '2024-01-01'"));
    EXPECT_EQ("TIMESTAMP '2024-01-01 00:00:00'",
              dump_expression("TIMESTAMP '2024-01-01 00:00:00'"));
}

TEST(ParserTest, ScalarSubqueryAndExists) {
    EXPECT_EQ("(subquery (query (spec (select (col (call max x))) (from (table t)))))",
              dump_expression("(SELECT max(x) FROM t)"));
    EXPECT_EQ("(exists (query (spec (select (col 1)) (from (table t)) (where (= a b)))))",
              dump_expression("EXISTS (SELECT 1 FROM t WHERE a = b)"));
}

TEST(ParserTest, TableAliases) {
    EXPECT_EQ("(query (spec (select *) (from (table t1) (alias (table t2) u (x y)))))",
              dump_statement("SELECT * FROM t1, t2 AS u (x, y)"));
}

TEST(ParserTest, Joins) {
    EXPECT_EQ("(query (spec (select *) (from (join CROSS (join LEFT (join INNER (table a) "
              "(table b) (on (= a.x b.x))) (table c) (using (id))) (table d)))))",
              dump_statement("SELECT * FROM a JOIN b ON a.x = b.x LEFT JOIN c USING (id) "
                             "CROSS JOIN d"));
    EXPECT_EQ("(query (spec (select *) (from (join NATURAL INNER (table a) (table b)))))",
              dump_statement("SELECT * FROM a NATURAL JOIN b"));
    EXPECT_EQ("(query (spec (select *) (from (join FULL (table a) (table b) (on true)))))",
              dump_statement("SELECT * FROM a FULL OUTER JOIN b ON true"));
}

TEST(ParserTest, TableSubquery) {
    EXPECT_EQ("(query (spec (select *) (from (alias (tsub (query (spec (select (col 1))))) "
              "t))))",
              dump_statement("SELECT * FROM (SELECT 1) t"));
}

TEST(ParserTest, Unnest) {
    EXPECT_EQ("(query (spec (select *) (from (alias (unnest-ord arr) u (x n)))))",
              dump_statement("SELECT * FROM UNNEST(arr) WITH ORDINALITY AS u (x, n)"));
    EXPECT_EQ("(query (spec (select *) (from (unnest a b))))",
              dump_statement("SELECT * FROM UNNEST(a, b)"));
}

TEST(ParserTest, FullQueryPipeline) {
    EXPECT_EQ("(query (spec (select (col k) (col (call count *) c)) (from (table t)) "
              "(where (> x 1)) (group k) (having (> (call count *) 2))) "
              "(order (item c desc)) (offset 5) (limit 10))",
              dump_statement("SELECT k, count(*) c FROM t WHERE x > 1 GROUP BY k "
                             "HAVING count(*) > 2 ORDER BY c DESC OFFSET 5 LIMIT 10"));
}

TEST(ParserTest, SelectDistinct) {
    EXPECT_EQ("(query (spec (select distinct (col a)) (from (table t))))",
              dump_statement("SELECT DISTINCT a FROM t"));
    EXPECT_EQ("(query (spec (select (col a)) (from (table t))))",
              dump_statement("SELECT ALL a FROM t"));
}

TEST(ParserTest, SetOperations) {
    EXPECT_EQ("(query (except (union all (spec (select (col 1))) (spec (select (col 2)))) "
              "(spec (select (col 3)))))",
              dump_statement("SELECT 1 UNION ALL SELECT 2 EXCEPT SELECT 3"));
    // INTERSECT binds tighter than UNION
    EXPECT_EQ("(query (union (spec (select (col 1))) "
              "(intersect (spec (select (col 2))) (spec (select (col 3))))))",
              dump_statement("SELECT 1 UNION SELECT 2 INTERSECT SELECT 3"));
    EXPECT_EQ("(query (query (union (spec (select (col 1))) (spec (select (col 2))))) "
              "(order (item 1)) (limit 1))",
              dump_statement("(SELECT 1 UNION SELECT 2) ORDER BY 1 LIMIT 1"));
}

TEST(ParserTest, WithClause) {
    EXPECT_EQ("(query (with (wq x (query (spec (select (col 1))))) "
              "(wq y (a b) (query (spec (select (col 2) (col 3)))))) "
              "(spec (select *) (from (table x))))",
              dump_statement("WITH x AS (SELECT 1), y (a, b) AS (SELECT 2, 3) "
                             "SELECT * FROM x"));
    EXPECT_EQ("(query (with recursive (wq r (query (spec (select (col 1)))))) "
              "(spec (select *) (from (table r))))",
              dump_statement("WITH RECURSIVE r AS (SELECT 1) SELECT * FROM r"));
}

TEST(ParserTest, Values) {
    EXPECT_EQ("(query (values (row 1 'a') (row 2 'b')))",
              dump_statement("VALUES (1, 'a'), (2, 'b')"));
    EXPECT_EQ("(query (spec (select *) (from (alias (tsub (query (values (row 1 2)))) v "
              "(x y)))))",
              dump_statement("SELECT * FROM (VALUES (1, 2)) AS v (x, y)"));
}

TEST(ParserTest, GroupByRollupAndCube) {
    EXPECT_EQ("(query (spec (select (col k)) (from (table t)) (group (call ROLLUP a b))))",
              dump_statement("SELECT k FROM t GROUP BY ROLLUP (a, b)"));
    EXPECT_EQ("(query (spec (select (col k)) (from (table t)) (group a (call CUBE b c))))",
              dump_statement("SELECT k FROM t GROUP BY a, CUBE (b, c)"));
}

TEST(ParserTest, LimitAll) {
    EXPECT_EQ("(query (spec (select (col x)) (from (table t))))",
              dump_statement("SELECT x FROM t LIMIT ALL"));
}

TEST(ParserTest, TrailingSemicolon) {
    EXPECT_EQ("(query (spec (select (col 1))))", dump_statement("SELECT 1;"));
}

TEST(ParserTest, StringLiteralVariants) {
    EXPECT_EQ("(query (spec (select (col 'it''s') (col X'ff') (col U&'d6'))))",
              dump_statement("SELECT 'it''s', X'ff', U&'d6'"));
}

TEST(ParserTest, Errors) {
    EXPECT_TRUE(has_error("SELECT FROM"));
    EXPECT_TRUE(has_error("SELECT 1 +"));
    EXPECT_TRUE(has_error("SELECT * FORM t"));
    EXPECT_TRUE(has_error("SELECT * FROM t WHERE"));
    EXPECT_TRUE(has_error("SELECT CASE WHEN x THEN 1 FROM t"));
    EXPECT_FALSE(has_error("SELECT 1"));
}

TEST(ParserTest, ErrorPosition) {
    Parser parser("SELECT 1 +");
    ParseResult result = parser.parse_statement();
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(1, result.errors[0].line);
    EXPECT_EQ(11, result.errors[0].column); // EOF after "SELECT 1 +"
}

TEST(ParserTest, TableStatement) {
    EXPECT_EQ("(query (table s.t))", dump_statement("TABLE s.t"));
    EXPECT_EQ("(query (union all (table t) (table u)))",
              dump_statement("TABLE t UNION ALL TABLE u"));
}

TEST(ParserTest, Lateral) {
    EXPECT_EQ("(query (spec (select *) (from (table t) (alias (lateral "
              "(query (spec (select (col a)) (from (table u)) (where (= u.id t.id))))) l))))",
              dump_statement("SELECT * FROM t, LATERAL (SELECT a FROM u WHERE u.id = t.id) l"));
}

TEST(ParserTest, TableSample) {
    EXPECT_EQ("(query (spec (select *) (from (sample BERNOULLI (table t) 50))))",
              dump_statement("SELECT * FROM t TABLESAMPLE BERNOULLI (50)"));
    EXPECT_EQ("(query (spec (select *) (from (alias (sample SYSTEM (table t) 25) s))))",
              dump_statement("SELECT * FROM t TABLESAMPLE SYSTEM (25) s"));
}

TEST(ParserTest, Explain) {
    EXPECT_EQ("(explain (query (spec (select (col 1)))))", dump_statement("EXPLAIN SELECT 1"));
    EXPECT_EQ("(explain analyze (query (spec (select (col 1)))))",
              dump_statement("EXPLAIN ANALYZE SELECT 1"));
    EXPECT_EQ("(explain analyze verbose (query (spec (select *) (from (table t)))))",
              dump_statement("EXPLAIN ANALYZE VERBOSE SELECT * FROM t"));
    EXPECT_EQ("(explain (opt TYPE LOGICAL) (opt FORMAT JSON) (query (spec (select (col 1)))))",
              dump_statement("EXPLAIN (TYPE LOGICAL, FORMAT JSON) SELECT 1"));
}

TEST(ParserTest, BooleanTest) {
    EXPECT_EQ("(istrue x)", dump_expression("x IS TRUE"));
    EXPECT_EQ("(notistrue x)", dump_expression("x IS NOT TRUE"));
    EXPECT_EQ("(isfalse x)", dump_expression("x IS FALSE"));
    EXPECT_EQ("(notisunknown x)", dump_expression("x IS NOT UNKNOWN"));
}

TEST(ParserTest, Trim) {
    EXPECT_EQ("(trim both ' abc ')", dump_expression("TRIM(' abc ')"));
    EXPECT_EQ("(trim both ' abc ')", dump_expression("TRIM(FROM ' abc ')"));
    EXPECT_EQ("(trim leading ' abc ')", dump_expression("TRIM(LEADING FROM ' abc ')"));
    EXPECT_EQ("(trim trailing ' ' ' abc ')", dump_expression("TRIM(TRAILING ' ' FROM ' abc ')"));
    EXPECT_EQ("(trim both 'x' col)", dump_expression("TRIM(BOTH 'x' FROM col)"));
}

TEST(ParserTest, SubstringSpecialForm) {
    EXPECT_EQ("(substr 'abc' 2)", dump_expression("SUBSTRING('abc' FROM 2)"));
    EXPECT_EQ("(substr 'abc' 2 1)", dump_expression("SUBSTRING('abc' FROM 2 FOR 1)"));
    EXPECT_EQ("(substr col a (+ b 1))", dump_expression("SUBSTRING(col FROM a FOR b + 1)"));
    EXPECT_EQ("(call SUBSTRING 'abc' 2 1)", dump_expression("SUBSTRING('abc', 2, 1)"));
}

TEST(ParserTest, PositionSpecialForm) {
    EXPECT_EQ("(position 'a' 'abc')", dump_expression("POSITION('a' IN 'abc')"));
    EXPECT_EQ("(position x y)", dump_expression("position(x in y)"));
}

TEST(ParserTest, OverlaySpecialForm) {
    EXPECT_EQ("(overlay 'abc' 'X' 2)", dump_expression("OVERLAY('abc' PLACING 'X' FROM 2)"));
    EXPECT_EQ("(overlay 'abc' 'XY' 2 1)",
              dump_expression("OVERLAY('abc' PLACING 'XY' FROM 2 FOR 1)"));
}

TEST(ParserTest, AtTimeZone) {
    EXPECT_EQ(
        "(attz TIMESTAMP '2012-10-31 01:00 UTC' 'America/Los_Angeles')",
        dump_expression("TIMESTAMP '2012-10-31 01:00 UTC' AT TIME ZONE 'America/Los_Angeles'"));
    EXPECT_EQ("(+ (attz x (interval '1' HOUR)) y)",
              dump_expression("x AT TIME ZONE INTERVAL '1' HOUR + y"));
}

TEST(ParserTest, QuantifiedComparison) {
    EXPECT_EQ("(qcmp = any x (query (spec (select (col y)) (from (table t)))))",
              dump_expression("x = ANY (SELECT y FROM t)"));
    EXPECT_EQ("(qcmp < some x (query (spec (select (col y)))))",
              dump_expression("x < SOME (SELECT y)"));
    EXPECT_EQ("(qcmp <> all x (query (spec (select (col y)))))",
              dump_expression("x <> ALL (SELECT y)"));
}

TEST(ParserTest, Parameter) {
    EXPECT_EQ("(query (spec (select (col ?) (col ?)) (from (table foo))))",
              dump_statement("SELECT ?, ? FROM foo"));
}

TEST(ParserTest, FilterClause) {
    EXPECT_EQ("(call array_agg x (filter (= x 1)))",
              dump_expression("array_agg(x) FILTER (WHERE x = 1)"));
}

TEST(ParserTest, AggregateOrderBy) {
    EXPECT_EQ("(call array_agg x (ord (item t.y)))", dump_expression("array_agg(x ORDER BY t.y)"));
}

TEST(ParserTest, NamedWindow) {
    EXPECT_EQ(
        "(query (spec (select (col (call rank (over w)))) (from (table t)) "
        "(win (wdef w (over (part x) (order (item y)))))))",
        dump_statement("SELECT rank() OVER w FROM t WINDOW w AS (PARTITION BY x ORDER BY y)"));
    EXPECT_EQ("(query (spec (select (col (call sum x (over (part a))))) (from (table t)) "
              "(win (wdef w (over (part a))) (wdef w2 (over (part b))))))",
              dump_statement("SELECT sum(x) OVER (PARTITION BY a) FROM t "
                             "WINDOW w AS (PARTITION BY a), w2 AS (PARTITION BY b)"));
}

TEST(ParserTest, FetchClause) {
    EXPECT_EQ("(query (spec (select *) (from (table t))) (fetch 2))",
              dump_statement("SELECT * FROM t FETCH FIRST 2 ROWS ONLY"));
    EXPECT_EQ("(query (spec (select *) (from (table t))) (fetch 3 ties))",
              dump_statement("SELECT * FROM t FETCH NEXT 3 ROWS WITH TIES"));
    EXPECT_EQ("(query (spec (select *) (from (table t))) (offset 2) (fetch 1))",
              dump_statement("SELECT * FROM t OFFSET 2 ROWS FETCH FIRST 1 ROW ONLY"));
}

TEST(ParserTest, GroupingSets) {
    EXPECT_EQ("(query (spec (select (col a)) (from (table t)) "
              "(group (call GROUPING SETS (row a b) (row) (row c)))))",
              dump_statement("SELECT a FROM t GROUP BY GROUPING SETS ((a, b), (), (c))"));
    EXPECT_EQ("(query (spec (select (col a)) (from (table t)) (group (row))))",
              dump_statement("SELECT a FROM t GROUP BY ()"));
}

TEST(ParserTest, Corresponding) {
    EXPECT_EQ("(query (union corresponding (spec (select (col a))) (spec (select (col a)))))",
              dump_statement("SELECT a UNION CORRESPONDING SELECT a"));
    EXPECT_EQ("(query (intersect corresponding (x y) (spec (select (col a))) "
              "(spec (select (col a)))))",
              dump_statement("SELECT a INTERSECT CORRESPONDING BY (x, y) SELECT a"));
}

TEST(ParserTest, GroupingOperation) {
    EXPECT_EQ("(grouping a b)", dump_expression("GROUPING(a, b)"));
    EXPECT_EQ("(query (spec (select (col (grouping a b))) (from (table t)) "
              "(group (call GROUPING SETS (row a) (row b)))))",
              dump_statement("SELECT GROUPING(a, b) FROM t GROUP BY GROUPING SETS ((a), (b))"));
}

TEST(ParserTest, NullifAsFunction) {
    EXPECT_EQ("(call nullif 42 87)", dump_expression("nullif(42, 87)"));
}

TEST(ParserTest, WindowInheritance) {
    EXPECT_EQ("(query (spec (select (col (call rank (over w)))) (from (table t)) "
              "(win (wdef w (over (part a))) (wdef w2 (over w (order (item b)))))))",
              dump_statement("SELECT rank() OVER w FROM t "
                             "WINDOW w AS (PARTITION BY a), w2 AS (w ORDER BY b)"));
}

TEST(ParserTest, OffsetNotSwallowedAsAlias) {
    EXPECT_EQ("(query (spec (select *) (from (table table1))) (offset 2))",
              dump_statement("SELECT * FROM table1 OFFSET 2 ROWS"));
    EXPECT_EQ("(query (spec (select *) (from (table table1))) (fetch 2))",
              dump_statement("SELECT * FROM table1 FETCH FIRST 2 ROWS ONLY"));
}

TEST(ParserTest, IntervalFieldPrecision) {
    EXPECT_EQ("(interval '1' YEAR(1))", dump_expression("INTERVAL '1' YEAR(1)"));
    EXPECT_EQ("(interval '1' YEAR(1) TO MONTH)", dump_expression("INTERVAL '1' YEAR(1) TO MONTH"));
    EXPECT_EQ("(interval '1' DAY(1) TO SECOND(2))",
              dump_expression("INTERVAL '1' DAY(1) TO SECOND(2)"));
    EXPECT_EQ("(interval '1' SECOND(1, 2))", dump_expression("INTERVAL '1' SECOND(1, 2)"));
}

TEST(ParserTest, DecimalTypedLiteral) {
    EXPECT_EQ("DECIMAL '12.34'", dump_expression("DECIMAL '12.34'"));
    EXPECT_EQ("DECIMAL '+.34'", dump_expression("DECIMAL '+.34'"));
    EXPECT_EQ("DECIMAL '-12'", dump_expression("DECIMAL '-12'"));
}

TEST(ParserTest, UnicodeStringWithUescape) {
    EXPECT_EQ("U&'hello!6d4B' UESCAPE '!'", dump_expression("U&'hello!6d4B' UESCAPE '!'"));
    EXPECT_EQ("U&'' UESCAPE ')'", dump_expression("U&'' UESCAPE ')'"));
    EXPECT_EQ("U&'hello\\8Bd5'", dump_expression("U&'hello\\8Bd5'"));
    for (std::string_view invalid : {"U&'hello\\8Bd5' UESCAPE ''",
                                     "U&'hello\\8Bd5' UESCAPE '%%'",
                                     "U&'hello\\8Bd5' UESCAPE ' '",
                                     "U&'hello\\8Bd5' UESCAPE '1'",
                                     "U&'hello\\8Bd5' UESCAPE '+'",
                                     "U&'hello\\8Bd5' UESCAPE ''''",
                                     "'abc' UESCAPE 'x'"}) {
        Parser parser(invalid);
        EXPECT_FALSE(parser.parse_expression().ok()) << invalid;
    }
}

TEST(ParserTest, BetweenSymmetry) {
    EXPECT_EQ("(between 1 2 3)", dump_expression("1 BETWEEN ASYMMETRIC 2 AND 3"));
    EXPECT_EQ("(between sym 1 2 3)", dump_expression("1 BETWEEN SYMMETRIC 2 AND 3"));
    EXPECT_EQ("(notbetween sym 1 2 3)", dump_expression("1 NOT BETWEEN SYMMETRIC 2 AND 3"));
}

TEST(ParserTest, AtLocal) {
    EXPECT_EQ("(atlocal TIMESTAMP '2012-10-31 01:00 UTC')",
              dump_expression("TIMESTAMP '2012-10-31 01:00 UTC' AT LOCAL"));
}

TEST(ParserTest, Listagg) {
    EXPECT_EQ("(listagg x (ord (item x)))",
              dump_expression("LISTAGG(x) WITHIN GROUP (ORDER BY x)"));
    EXPECT_EQ("(listagg distinct x (ord (item x)))",
              dump_expression("LISTAGG( DISTINCT x) WITHIN GROUP (ORDER BY x)"));
    EXPECT_EQ("(listagg x ',' (ord (item y)))",
              dump_expression("LISTAGG(x, ',') WITHIN GROUP (ORDER BY y)"));
    EXPECT_EQ("(listagg x ',' (overflow error) (ord (item x)))",
              dump_expression("LISTAGG(x, ',' ON OVERFLOW ERROR) WITHIN GROUP (ORDER BY x)"));
    EXPECT_EQ("(listagg x ',' (overflow truncate with) (ord (item x)))",
              dump_expression(
                  "LISTAGG(x, ',' ON OVERFLOW TRUNCATE WITH COUNT) WITHIN GROUP (ORDER BY x)"));
    EXPECT_EQ("(listagg x ',' (overflow truncate 'HIDDEN' without) (ord (item x)))",
              dump_expression("LISTAGG(x, ',' ON OVERFLOW TRUNCATE 'HIDDEN' WITHOUT COUNT) "
                              "WITHIN GROUP (ORDER BY x)"));
}

TEST(ParserTest, GroupByQuantifier) {
    EXPECT_EQ("(query (spec (select *) (from (table table1)) (group (auto))))",
              dump_statement("SELECT * FROM table1 GROUP BY ALL AUTO"));
    EXPECT_EQ("(query (spec (select *) (from (table table1)) (group distinct (auto))))",
              dump_statement("SELECT * FROM table1 GROUP BY DISTINCT AUTO"));
    EXPECT_EQ(
        "(query (spec (select *) (from (table table1)) "
        "(group (call GROUPING SETS (row a b) (row a) (row)) (call CUBE c) (call ROLLUP d))))",
        dump_statement("SELECT * FROM table1 GROUP BY ALL GROUPING SETS ((a, b), (a), ()), "
                       "CUBE (c), ROLLUP (d)"));
    EXPECT_EQ("(query (spec (select (col all)) (from (table t)) (group all)))",
              dump_statement("SELECT all FROM t GROUP BY all"));
}

TEST(ParserTest, AllSomeAnyAsIdentifiers) {
    EXPECT_EQ("(query (spec (select (col ALL) (col SOME) (col ANY)) (from (table t))))",
              dump_statement("SELECT ALL, SOME, ANY FROM t"));
    EXPECT_EQ("(query (spec (select (col x)) (from (table t))))",
              dump_statement("SELECT ALL x FROM t"));
    EXPECT_EQ("(query (spec (select (col all)) (from (table t))))",
              dump_statement("SELECT all FROM t"));
    EXPECT_EQ("(qcmp = all x (query (spec (select (col y)) (from (table t)))))",
              dump_expression("x = ALL (SELECT y FROM t)"));
}

TEST(ParserTest, MatchPredicate) {
    EXPECT_EQ("(match (row a b) (query (spec (select (col x) (col y)) (from (table t)))))",
              dump_expression("ROW(a, b) MATCH (SELECT x, y FROM t)"));
    EXPECT_EQ("(match simple (row a) (query (spec (select (col x)) (from (table t)))))",
              dump_expression("ROW(a) MATCH SIMPLE (SELECT x FROM t)"));
    EXPECT_EQ("(match partial (row a) (query (spec (select (col x)) (from (table t)))))",
              dump_expression("ROW(a) MATCH PARTIAL (SELECT x FROM t)"));
    EXPECT_EQ("(match unique (row a) (query (spec (select (col x)) (from (table t)))))",
              dump_expression("ROW(a) MATCH UNIQUE (SELECT x FROM t)"));
    EXPECT_EQ(
        "(match unique full (row a b) (query (spec (select (col x) (col y)) (from (table t)))))",
        dump_expression("ROW(a, b) MATCH UNIQUE FULL (SELECT x, y FROM t)"));
}

TEST(ParserTest, PartialWhenClause) {
    EXPECT_EQ("(case x (when (> _ 5) 'big') (when 0 'zero'))",
              dump_expression("CASE x WHEN > 5 THEN 'big' WHEN 0 THEN 'zero' END"));
    EXPECT_EQ("(case x (when (notbetween _ 1 4) 'a') (when (notin _ (list 0)) 'b') "
              "(when (notlike _ 'p') 'c') (when (notnull _) 'd') "
              "(when (isnotdistinct _ 1) 'e') (else 'f'))",
              dump_expression("CASE x WHEN NOT BETWEEN 1 AND 4 THEN 'a' WHEN NOT IN (0) THEN 'b' "
                              "WHEN NOT LIKE 'p' THEN 'c' WHEN IS NOT NULL THEN 'd' "
                              "WHEN IS NOT DISTINCT FROM 1 THEN 'e' ELSE 'f' END"));
    EXPECT_EQ("(case x (when (isnull _) 'unk') (when (like _ 'a%') 'a') (else 'other'))",
              dump_expression("CASE x WHEN IS NULL THEN 'unk' WHEN LIKE 'a%' THEN 'a' "
                              "ELSE 'other' END"));
}

TEST(ParserTest, RowDereferenceStar) {
    EXPECT_EQ("(query (spec (select (row 1 'a' true).*)))",
              dump_statement("SELECT ROW (1, 'a', true).*"));
    EXPECT_EQ("(query (spec (select (row 1 'a' true).* (as (f1 f2 f3)))))",
              dump_statement("SELECT ROW (1, 'a', true).* AS (f1, f2, f3)"));
    {
        Parser parser("SELECT 1 + A.*");
        EXPECT_FALSE(parser.parse_statement().ok());
    }
}

} // namespace
} // namespace pl::prism::syntax
