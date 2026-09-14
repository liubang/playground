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
// Created: 2026/09/14 17:05

// Benchmark for the Prism pipeline: lex / parse / parse+print / parse+plan.
//
// The workloads are fully synthetic, generated deterministically from a fixed
// seed. They imitate the SHAPE of long dashboard/report-style SQL (wide
// select lists, multi-way joins, aggregates, window functions, UNION ALL
// blocks, nested subqueries) without any real schema, table or column names:
// tables are fact_f{0..3} / dim_d{0..7}, columns k*/m*/s*/ts0.
//
// Run with optimizations and without sanitizers:
//   bazel run //cpp/pl/prism/benchmark:parser_benchmark --config=release

#define ANKERL_NANOBENCH_IMPLEMENT
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <nanobench.h>
#include <random>
#include <string>
#include <vector>

#include "cpp/pl/prism/plan/ast_to_plan.h"
#include "cpp/pl/prism/printer/sql_printer.h"
#include "cpp/pl/prism/syntax/lexer.h"
#include "cpp/pl/prism/syntax/parser.h"

namespace {

namespace syn = pl::prism::syntax;

// ---------------------------------------------------------------------------
// Synthetic workload generator
// ---------------------------------------------------------------------------

class Gen {
public:
    explicit Gen(uint64_t seed) : rng_(seed) {}

    int pick(int lo, int hi) {
        std::uniform_int_distribution<int> dist(lo, hi);
        return dist(rng_);
    }

    std::string dim() { return "k" + std::to_string(pick(0, 15)); }
    std::string measure() { return "m" + std::to_string(pick(0, 15)); }
    std::string str_col() { return "s" + std::to_string(pick(0, 7)); }

    std::string col(const std::string& alias, bool want_measure) {
        const std::string c = want_measure ? measure() : dim();
        return alias.empty() ? c : alias + "." + c;
    }

    std::string number_list(int n) {
        std::string out = std::to_string(pick(0, 9999));
        for (int i = 1; i < n; ++i) {
            out += ", " + std::to_string(pick(0, 9999));
        }
        return out;
    }

    std::string predicate(const std::string& alias, int depth) {
        if (depth <= 0) {
            switch (pick(0, 4)) {
                case 0:
                    return col(alias, false) + " > " + std::to_string(pick(0, 100000));
                case 1:
                    return col(alias, false) + " BETWEEN " + std::to_string(pick(0, 500)) +
                           " AND " + std::to_string(pick(501, 9999));
                case 2:
                    return col(alias, false) + " IN (" + number_list(pick(3, 12)) + ")";
                case 3:
                    return col(alias, false) + " IS NOT NULL";
                default:
                    return (alias.empty() ? str_col() : alias + "." + str_col()) + " LIKE 'p%'";
            }
        }
        switch (pick(0, 2)) {
            case 0:
                return "(" + predicate(alias, depth - 1) + " AND " + predicate(alias, depth - 1) +
                       ")";
            case 1:
                return "(" + predicate(alias, depth - 1) + " OR " + predicate(alias, depth - 1) +
                       ")";
            default:
                return "NOT (" + predicate(alias, depth - 1) + ")";
        }
    }

    std::string scalar(const std::string& alias, int depth) {
        if (depth <= 0) {
            if (pick(0, 3) == 0) {
                return std::to_string(pick(0, 100000));
            }
            return col(alias, pick(0, 1) == 0);
        }
        switch (pick(0, 5)) {
            case 0: {
                const char* ops[] = {" + ", " - ", " * ", " / "};
                return "(" + scalar(alias, depth - 1) + ops[pick(0, 3)] + scalar(alias, depth - 1) +
                       ")";
            }
            case 1:
                return "coalesce(" + scalar(alias, depth - 1) + ", " + scalar(alias, depth - 1) +
                       ")";
            case 2:
                return "CASE WHEN " + predicate(alias, 1) + " THEN " + scalar(alias, depth - 1) +
                       " ELSE " + scalar(alias, depth - 1) + " END";
            case 3:
                return "CAST(" + scalar(alias, depth - 1) + " AS DECIMAL(18, 2))";
            case 4:
                return "round(" + scalar(alias, depth - 1) + ", 2)";
            default:
                return "(" + scalar(alias, depth - 1) + " || " + scalar(alias, depth - 1) + ")";
        }
    }

    std::string aggregate(const std::string& alias) {
        switch (pick(0, 5)) {
            case 0:
                return "sum(" + col(alias, true) + ")";
            case 1:
                return "count(*)";
            case 2:
                return "count(DISTINCT " + (alias.empty() ? str_col() : alias + "." + str_col()) +
                       ")";
            case 3:
                return "avg(" + col(alias, true) + ")";
            case 4:
                return "sum(" + col(alias, true) + ") FILTER (WHERE " + predicate(alias, 1) + ")";
            default:
                return "sum(CASE WHEN " + predicate(alias, 1) + " THEN " + col(alias, true) +
                       " ELSE 0 END)";
        }
    }

    std::string window(const std::string& alias) {
        switch (pick(0, 2)) {
            case 0:
                return "row_number() OVER (PARTITION BY " + col(alias, false) + " ORDER BY " +
                       col(alias, false) + " DESC)";
            case 1:
                return "sum(" + col(alias, true) + ") OVER (PARTITION BY " + col(alias, false) +
                       " ORDER BY " + (alias.empty() ? "ts0" : alias + ".ts0") +
                       " ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)";
            default:
                return "rank() OVER (ORDER BY " + col(alias, true) + " DESC)";
        }
    }

    // Wide dashboard-style query: many select items, multi-way joins, an
    // inline aggregated derived table, big IN lists, GROUP BY, HAVING,
    // ORDER BY, LIMIT.
    std::string dashboard(int width, int joins, int group_keys) {
        std::string sql = "SELECT ";
        for (int i = 0; i < width; ++i) {
            if (i > 0) {
                sql += ", ";
            }
            switch (pick(0, 9)) {
                case 0:
                case 1:
                case 2:
                    sql += "f." + dim() + " AS c" + std::to_string(i);
                    break;
                case 3:
                case 4:
                case 5:
                    sql += aggregate("f") + " AS a" + std::to_string(i);
                    break;
                case 6:
                case 7:
                    sql += scalar("f", pick(1, 3)) + " AS e" + std::to_string(i);
                    break;
                case 8:
                    sql += window("f") + " AS w" + std::to_string(i);
                    break;
                default:
                    sql += "CASE WHEN " + aggregate("f") + " > " + std::to_string(pick(0, 10000)) +
                           " THEN 'h' ELSE 'l' END AS s" + std::to_string(i);
                    break;
            }
        }
        sql += " FROM fact_f0 f";
        for (int j = 0; j < joins; ++j) {
            const std::string a = "d" + std::to_string(j);
            const int key = pick(0, 15);
            sql += (pick(0, 1) == 0 ? " LEFT JOIN dim_d" : " INNER JOIN dim_d") +
                   std::to_string(j % 8) + " " + a + " ON f.k" + std::to_string(key) + " = " + a +
                   ".k" + std::to_string(key);
        }
        sql += " JOIN (SELECT k0, sum(m0) AS sub_m0, count(*) AS sub_c0 FROM fact_f1 "
               "WHERE ts0 >= DATE '2024-01-01' GROUP BY k0) sub0 ON sub0.k0 = f.k0";
        sql += " WHERE f.ts0 BETWEEN DATE '2024-01-01' AND DATE '2024-12-31' AND f.k2 IN (" +
               number_list(40) + ") AND f.k3 NOT IN (" + number_list(10) + ") AND f.m0 > 0";
        sql += " GROUP BY ";
        for (int g = 0; g < group_keys; ++g) {
            if (g > 0) {
                sql += ", ";
            }
            if (pick(0, 3) == 0 && joins > 0) {
                sql += "d" + std::to_string(pick(0, joins - 1)) + "." + str_col();
            } else {
                sql += "f." + dim();
            }
        }
        sql += " HAVING count(*) > 1 AND sum(f.m0) > 0";
        sql += " ORDER BY a0 DESC NULLS LAST, c1 ASC, e2 DESC LIMIT 1000";
        return sql;
    }

    // Report-style stacked UNION ALL of similar aggregate blocks.
    std::string union_report(int blocks, int aggs_per_block) {
        std::string sql;
        for (int b = 0; b < blocks; ++b) {
            if (b > 0) {
                sql += " UNION ALL ";
            }
            const int key = pick(0, 15);
            sql += "SELECT f." + dim() + " AS g0, 'block" + std::to_string(b) + "' AS src";
            for (int a = 0; a < aggs_per_block; ++a) {
                sql += ", " + aggregate("f") + " AS v" + std::to_string(a);
            }
            sql += " FROM fact_f" + std::to_string(b % 4) + " f JOIN dim_d" +
                   std::to_string(b % 8) + " d ON f.k" + std::to_string(key) + " = d.k" +
                   std::to_string(key);
            sql += " WHERE f.ts0 >= DATE '2024-01-01' AND f.k" + std::to_string(pick(0, 15)) +
                   " IN (" + number_list(8) + ") AND f." + measure() + " > 0 GROUP BY f." + dim();
        }
        sql += " ORDER BY v0 DESC LIMIT 500";
        return sql;
    }

    // Expression-heavy query: a handful of very deep scalar expressions.
    std::string expr_deep(int width, int depth) {
        std::string sql = "SELECT ";
        for (int i = 0; i < width; ++i) {
            if (i > 0) {
                sql += ", ";
            }
            sql += scalar("", depth) + " AS e" + std::to_string(i);
        }
        sql += " FROM fact_f0 WHERE " + predicate("", depth / 2);
        return sql;
    }

    // Deeply nested IN / EXISTS subqueries.
    std::string subquery_nest(int depth) {
        return "SELECT k0, m0 FROM fact_f0 f WHERE f.k1 IN (" + nest(depth, 1) +
               ") ORDER BY k0 LIMIT 100";
    }

private:
    std::string nest(int depth, int seq) {
        if (depth <= 0) {
            return "SELECT k0 FROM dim_d" + std::to_string(pick(0, 7)) + " WHERE m0 > " +
                   std::to_string(pick(0, 1000));
        }
        const std::string self = "fact_f" + std::to_string(seq % 4);
        const std::string key = "k" + std::to_string(pick(1, 15));
        std::string sql = "SELECT " + key + " FROM " + self + " WHERE " + key + " IN (" +
                          nest(depth - 1, seq + 1) + ")";
        sql += " AND EXISTS (SELECT 1 FROM dim_d" + std::to_string(pick(0, 7)) + " d WHERE d." +
               key + " = " + self + "." + key + " AND d.m0 > (SELECT avg(m" +
               std::to_string(pick(0, 15)) + ") FROM fact_f" + std::to_string((seq + 2) % 4) +
               " WHERE " + dim() + " IN (" + number_list(5) + ")))";
        return sql;
    }

    std::mt19937_64 rng_;
};

struct Workload {
    std::string name;
    std::string sql;
};

bool validate(const Workload& w) {
    syn::Parser parser(w.sql);
    const syn::ParseResult parsed = parser.parse_statement();
    if (!parsed.ok()) {
        std::cerr << "workload " << w.name << " failed to parse: " << parsed.errors[0].message
                  << '\n';
        return false;
    }
    const std::string printed = pl::prism::printer::print(parsed.root);
    if (printed.empty()) {
        std::cerr << "workload " << w.name << " failed to print\n";
        return false;
    }
    pl::prism::plan::AstToPlan planner;
    const pl::prism::plan::PlanResult plan = planner.translate(parsed.root);
    if (!plan.ok()) {
        std::cerr << "workload " << w.name << " failed to plan: " << plan.errors[0].message << '\n';
        return false;
    }
    return true;
}

} // namespace

int main() {
    Gen gen(20260914);
    const std::vector<Workload> workloads = {
        {"dashboard_s", gen.dashboard(12, 2, 3)},
        {"dashboard_m", gen.dashboard(60, 4, 6)},
        {"dashboard_l", gen.dashboard(260, 6, 10)},
        {"union_report_m", gen.union_report(40, 6)},
        {"union_report_l", gen.union_report(160, 8)},
        {"expr_deep", gen.expr_deep(24, 11)},
        {"subquery_nest", gen.subquery_nest(10)},
    };

    for (const Workload& w : workloads) {
        if (!validate(w)) {
            return 1;
        }
    }

    std::cout << "| workload | bytes |\n|---|---:|\n";
    for (const Workload& w : workloads) {
        std::cout << "| " << w.name << " | " << w.sql.size() << " |\n";
    }
    std::cout << '\n';

    for (const Workload& w : workloads) {
        const double bytes = static_cast<double>(w.sql.size());

        ankerl::nanobench::Bench().unit("B").batch(bytes).run("lex/" + w.name, [&] {
            syn::Lexer lexer(w.sql);
            uint64_t count = 0;
            for (;;) {
                const syn::Token token = lexer.next_token();
                if (token.type == syn::TokenType::kEof) {
                    break;
                }
                ++count;
            }
            ankerl::nanobench::doNotOptimizeAway(count);
        });

        ankerl::nanobench::Bench().unit("B").batch(bytes).run("parse/" + w.name, [&] {
            syn::Parser parser(w.sql);
            syn::ParseResult result = parser.parse_statement();
            ankerl::nanobench::doNotOptimizeAway(result.root);
        });

        ankerl::nanobench::Bench().unit("B").batch(bytes).run("parse+print/" + w.name, [&] {
            syn::Parser parser(w.sql);
            syn::ParseResult result = parser.parse_statement();
            ankerl::nanobench::doNotOptimizeAway(pl::prism::printer::print(result.root));
        });

        ankerl::nanobench::Bench().unit("B").batch(bytes).run("parse+plan/" + w.name, [&] {
            syn::Parser parser(w.sql);
            syn::ParseResult result = parser.parse_statement();
            pl::prism::plan::AstToPlan planner;
            ankerl::nanobench::doNotOptimizeAway(planner.translate(result.root).root);
        });
    }
    return 0;
}
