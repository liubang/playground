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
// Created: 2026/09/14 15:20

#include <string>
#include <string_view>

#include "cpp/pl/prism/plan/ast_to_plan.h"
#include "cpp/pl/prism/plan/plan_dump.h"
#include "cpp/pl/prism/syntax/parser.h"
#include "gtest/gtest.h"

namespace pl::prism::plan {
namespace {

// Parses a statement, translates it to a logical plan and dumps the plan.
std::string plan_of(std::string_view sql) {
    syntax::Parser parser(sql);
    const syntax::ParseResult parsed = parser.parse_statement();
    EXPECT_TRUE(parsed.ok()) << sql;
    AstToPlan planner;
    const PlanResult result = planner.translate(parsed.root);
    EXPECT_TRUE(result.ok()) << sql << " -> "
                             << (result.errors.empty() ? "" : result.errors[0].message);
    return dump(result.root);
}

// Translates and returns the first plan error; the translation must fail.
std::string plan_error(std::string_view sql) {
    syntax::Parser parser(sql);
    const syntax::ParseResult parsed = parser.parse_statement();
    EXPECT_TRUE(parsed.ok()) << sql;
    AstToPlan planner;
    const PlanResult result = planner.translate(parsed.root);
    EXPECT_FALSE(result.ok()) << sql;
    return result.errors.empty() ? "" : result.errors[0].message;
}

TEST(AstToPlan, FullQuerySpecificationChain) {
    EXPECT_EQ("(limit (count 10) (offset 5) "
              "(sort (items (item c desc)) "
              "(project (col a) (col (+ b 1) c) "
              "(filter (> (call count *) 1) "
              "(agg (keys a) "
              "(filter (> a 0) (scan t)))))))",
              plan_of("SELECT a, b + 1 AS c FROM t WHERE a > 0 GROUP BY a "
                      "HAVING count(*) > 1 ORDER BY c DESC OFFSET 5 LIMIT 10"));
}

TEST(AstToPlan, JoinTree) {
    EXPECT_EQ("(project * (join LEFT (join INNER (scan a) (scan b) (on (= a.x b.x))) "
              "(scan c) (using (y))))",
              plan_of("SELECT * FROM a JOIN b ON a.x = b.x LEFT JOIN c USING (y)"));
    // A multi-relation FROM list is a chain of cross joins.
    EXPECT_EQ("(project * (join CROSS (scan a) (scan b)))", plan_of("SELECT * FROM a, b"));
    EXPECT_EQ("(project * (join CROSS (scan t) (unnest arr)))",
              plan_of("SELECT * FROM t, UNNEST(arr) AS u"));
}

TEST(AstToPlan, TableScanAlias) {
    EXPECT_EQ("(project * (scan t AS u))", plan_of("SELECT * FROM t AS u"));
    EXPECT_EQ("(project * (scan t AS u (x y)))", plan_of("SELECT * FROM t u(x, y)"));
}

TEST(AstToPlan, SetOperations) {
    EXPECT_EQ("(limit (count 3) (sort (items (item 1)) "
              "(union all (project (col 1) _) (project (col 2) _))))",
              plan_of("SELECT 1 UNION ALL SELECT 2 ORDER BY 1 LIMIT 3"));
    EXPECT_EQ("(intersect (union (project * (scan b)) (project * (scan c))) (project * (scan a)))",
              plan_of("(SELECT * FROM b UNION SELECT * FROM c) INTERSECT SELECT * FROM a"));
}

TEST(AstToPlan, ValuesAndTableCommand) {
    EXPECT_EQ("(values (row 1 2) (row 3 4))", plan_of("VALUES (1, 2), (3, 4)"));
    EXPECT_EQ("(scan t)", plan_of("TABLE t"));
    EXPECT_EQ("(scan catalog.schema.t)", plan_of("TABLE catalog.schema.t"));
}

TEST(AstToPlan, WindowExtraction) {
    EXPECT_EQ("(project (col (call sum x (over (part y) (order (item z)))) s) "
              "(col (call count * (over w))) "
              "(window (fns (call sum x (over (part y) (order (item z)))) "
              "(call count * (over w))) (defs w) (scan t)))",
              plan_of("SELECT sum(x) OVER (PARTITION BY y ORDER BY z) AS s, count(*) OVER w "
                      "FROM t WINDOW w AS (PARTITION BY q)"));
    // Windowed calls nested inside scalar expressions are collected too.
    EXPECT_EQ("(project (col (+ (call rank (over (order (item x)))) 1)) "
              "(window (fns (call rank (over (order (item x))))) (scan t)))",
              plan_of("SELECT rank() OVER (ORDER BY x) + 1 FROM t"));
}

TEST(AstToPlan, NoFromClause) {
    EXPECT_EQ("(project (col 1) _)", plan_of("SELECT 1"));
    EXPECT_EQ("(project (col 1) (filter true _))", plan_of("SELECT 1 WHERE true"));
}

TEST(AstToPlan, DistinctAndGrouping) {
    EXPECT_EQ("(project distinct (col a) (scan t))", plan_of("SELECT DISTINCT a FROM t"));
    EXPECT_EQ("(project (col a) (agg distinct (keys a) (scan t)))",
              plan_of("SELECT a FROM t GROUP BY DISTINCT a"));
    EXPECT_EQ("(project (col (call count *)) (agg (keys (call ROLLUP a b)) (scan t)))",
              plan_of("SELECT count(*) FROM t GROUP BY ROLLUP(a, b)"));
}

TEST(AstToPlan, AliasedSubqueryPassesThrough) {
    // The alias of a non-table relation is scoping information: dropped.
    EXPECT_EQ("(project * (filter (> a 0) (project (col a) (scan t))))",
              plan_of("SELECT * FROM (SELECT a FROM t) AS u WHERE a > 0"));
}

TEST(AstToPlan, LimitForms) {
    EXPECT_EQ("(project * (scan t))",
              plan_of("SELECT * FROM t LIMIT ALL")); // LIMIT ALL is a no-op
    EXPECT_EQ("(limit (offset 5) (project * (scan t)))", plan_of("SELECT * FROM t OFFSET 5"));
    EXPECT_EQ("(limit (count 3) ties (project * (scan t)))",
              plan_of("SELECT * FROM t FETCH FIRST 3 ROWS WITH TIES"));
}

TEST(AstToPlan, UnsupportedConstructs) {
    EXPECT_EQ("WITH is not supported by the plan transform yet",
              plan_error("WITH t AS (SELECT 1) SELECT * FROM t"));
    EXPECT_EQ("LATERAL is not supported by the plan transform yet",
              plan_error("SELECT * FROM LATERAL (SELECT 1) AS l"));
    EXPECT_EQ("TABLESAMPLE is not supported by the plan transform yet",
              plan_error("SELECT * FROM t TABLESAMPLE BERNOULLI (50)"));
    EXPECT_EQ("set-operation CORRESPONDING is not supported yet",
              plan_error("SELECT 1 UNION CORRESPONDING SELECT 1"));
    EXPECT_EQ("the (expr).* select form is not supported in plans yet",
              plan_error("SELECT (ROW(1, 2)).* FROM t"));
    EXPECT_EQ("column aliases on a non-table relation are not supported yet",
              plan_error("SELECT * FROM (SELECT 1) AS u(x)"));
}

TEST(AstToPlan, RejectsNonQuery) {
    EXPECT_EQ("plan transform expects a Query node", plan_error("EXPLAIN SELECT 1"));
}

} // namespace
} // namespace pl::prism::plan
