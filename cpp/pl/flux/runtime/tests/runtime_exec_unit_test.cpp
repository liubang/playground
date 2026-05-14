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
// Created: 2026/04/15 00:47

#include "cpp/pl/flux/execution/materializer.h"
#include "cpp/pl/flux/execution/physical_executor.h"
#include "cpp/pl/flux/optimizer/explain.h"
#include "cpp/pl/flux/runtime/runtime_builtin.h"
#include "cpp/pl/flux/runtime/runtime_exec.h"
#include "cpp/pl/flux/syntax/parser.h"
#include <cstdlib>
#include <gtest/gtest.h>
#include <optional>

namespace pl::flux {
namespace {

std::unique_ptr<File> ParseFile(const std::string& source) {
    Parser parser(source);
    auto file = parser.parse_file("exec_test.flux");
    EXPECT_TRUE(parser.errors().empty()) << ::testing::PrintToString(parser.errors());
    return file;
}

std::optional<std::string> mysql_test_dsn() {
    const char* dsn = std::getenv("FLUX_MYSQL_TEST_DSN");
    if (dsn == nullptr || std::string(dsn).empty()) {
        return std::nullopt;
    }
    return std::string(dsn);
}

std::shared_ptr<plan::PlanNode> SqliteCpuScanPlan() {
    return plan::MakeSourceScan("sqlite", "sqlite", "cpp/pl/flux/examples/cross_source/metrics.db",
                                "cpu");
}

TEST(RuntimeExecTest, ExecutesVariableAndExpressionStatements) {
    auto file = ParseFile(R"(
        value = 1 + 2
        value
    )");
    ASSERT_NE(file, nullptr);
    ASSERT_EQ(2, file->body.size());

    Environment env;
    auto assign_or = StatementExecutor::Execute(*file->body[0], env);
    ASSERT_TRUE(assign_or.ok()) << assign_or.status();
    EXPECT_EQ("3", assign_or->value.string());
    ASSERT_TRUE(env.lookup("value").ok());
    EXPECT_EQ("3", env.lookup("value")->string());

    auto expr_or = StatementExecutor::Execute(*file->body[1], env);
    ASSERT_TRUE(expr_or.ok()) << expr_or.status();
    EXPECT_EQ(ExecutionResult::Type::Normal, expr_or->type);
    EXPECT_EQ("3", expr_or->value.string());
}

TEST(RuntimeExecTest, ExecutesOptionStatementsIncludingMemberAssignments) {
    auto file = ParseFile(R"(
        option task = {name: "cpu", every: 5m}
        option task.offset = 30s
    )");
    ASSERT_NE(file, nullptr);
    ASSERT_EQ(2, file->body.size());

    Environment env;
    auto task_or = StatementExecutor::Execute(*file->body[0], env);
    ASSERT_TRUE(task_or.ok()) << task_or.status();
    ASSERT_TRUE(env.lookup_option("task").ok());
    EXPECT_EQ("{name: \"cpu\", every: 5m}", env.lookup_option("task")->string());

    auto offset_or = StatementExecutor::Execute(*file->body[1], env);
    ASSERT_TRUE(offset_or.ok()) << offset_or.status();
    ASSERT_TRUE(env.lookup_option("task.offset").ok());
    EXPECT_EQ("30s", env.lookup_option("task.offset")->string());
}

TEST(RuntimeExecTest, ExecutesBlocksAndStopsAtReturn) {
    auto file = ParseFile(R"(
        testcase t {
            return 1 + 2
            value = 42
        }
    )");
    ASSERT_NE(file, nullptr);
    ASSERT_EQ(1, file->body.size());

    const auto& testcase = std::get<std::unique_ptr<TestCaseStmt>>(file->body[0]->stmt);
    ASSERT_NE(testcase, nullptr);
    ASSERT_NE(testcase->block, nullptr);

    Environment env;
    auto block_or = StatementExecutor::ExecuteBlock(*testcase->block, env);
    ASSERT_TRUE(block_or.ok()) << block_or.status();
    EXPECT_EQ(ExecutionResult::Type::Return, block_or->type);
    EXPECT_EQ("3", block_or->value.string());
    EXPECT_FALSE(env.lookup("value").ok());
}

TEST(RuntimeExecTest, ExecutesTestCaseStatementsInIsolatedScopeAndStoresResult) {
    auto file = ParseFile(R"(
        seed = 2
        testcase math extends "base" {
            temp = seed + 1
            return temp * 2
        }
    )");
    ASSERT_NE(file, nullptr);
    ASSERT_EQ(2, file->body.size());

    Environment env;
    auto seed_or = StatementExecutor::Execute(*file->body[0], env);
    ASSERT_TRUE(seed_or.ok()) << seed_or.status();

    auto testcase_or = StatementExecutor::Execute(*file->body[1], env);
    ASSERT_TRUE(testcase_or.ok()) << testcase_or.status();
    EXPECT_EQ(ExecutionResult::Type::Normal, testcase_or->type);
    EXPECT_EQ("{name: \"math\", success: true, extends: \"base\", value: 6}",
              testcase_or->value.string());
    EXPECT_FALSE(env.lookup("temp").ok());

    auto result_or = env.lookup_option("__flux.testcase.math");
    ASSERT_TRUE(result_or.ok()) << result_or.status();
    EXPECT_EQ("{name: \"math\", success: true, extends: \"base\", value: 6}", result_or->string());
}

TEST(RuntimeExecTest, ExecutesBuiltinStatementsForRegisteredBuiltins) {
    auto file = ParseFile(R"(
        builtin len : (a: int) => int
        result = len([1, 2, 3])
    )");
    ASSERT_NE(file, nullptr);
    ASSERT_EQ(2, file->body.size());

    Environment env;
    auto builtin_or = StatementExecutor::Execute(*file->body[0], env);
    ASSERT_TRUE(builtin_or.ok()) << builtin_or.status();
    ASSERT_TRUE(env.lookup("len").ok());
    EXPECT_EQ(Value::Type::Function, env.lookup("len")->type());

    auto result_or = StatementExecutor::Execute(*file->body[1], env);
    ASSERT_TRUE(result_or.ok()) << result_or.status();
    EXPECT_EQ("3", result_or->value.string());
}

TEST(RuntimeExecTest, ExecutesRegisteredAggregateBuiltinsAfterDeclaration) {
    auto file = ParseFile(R"(
        builtin mean : (values: [int]) => float
        result = [1, 2, 3, 4] |> mean()
    )");
    ASSERT_NE(file, nullptr);
    ASSERT_EQ(2, file->body.size());

    Environment env;
    auto builtin_or = StatementExecutor::Execute(*file->body[0], env);
    ASSERT_TRUE(builtin_or.ok()) << builtin_or.status();

    auto result_or = StatementExecutor::Execute(*file->body[1], env);
    ASSERT_TRUE(result_or.ok()) << result_or.status();
    EXPECT_EQ("2.5", result_or->value.string());
}

TEST(RuntimeExecTest, ExecutesCsvFromRawStringImportedPackage) {
    auto file = ParseFile(R"(
        import "csv"
        builtin filter : (<-tables: stream[A], fn: (r: A) => bool) => stream[A]
        builtin limit : (<-tables: stream[A], n: int) => stream[A]

        data = csv.from(
            csv: "_time,_measurement,_value\n2024-01-01T00:00:00Z,cpu,95.5\n2024-01-01T00:01:00Z,cpu,80.0\n",
            mode: "raw",
        )
            |> filter(fn: (r) => r._measurement == "cpu")
            |> limit(n: 1)
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("csv").ok());
    ASSERT_TRUE(env.lookup("data").ok());
    ASSERT_EQ(Value::Type::Table, env.lookup("data")->type());
    ASSERT_EQ(1, env.lookup("data")->as_table().rows.size());
    ASSERT_NE(nullptr, env.lookup("data")->as_table().rows[0]);
    EXPECT_EQ("\"95.5\"", env.lookup("data")->as_table().rows[0]->lookup("_value")->string());
}

TEST(RuntimeExecTest, ExecutesSqliteFromSqliteTableThroughMemoryPipeline) {
    auto file = ParseFile(R"(
        import "sqlite"
        builtin filter : (<-tables: stream[A], fn: (r: A) => bool) => stream[A]
        builtin limit : (<-tables: stream[A], n: int) => stream[A]

        data = sqlite.from(
    path: "cpp/pl/flux/examples/cross_source/metrics.db",
            table: "cpu",
        )
            |> filter(fn: (r) => r.host == "edge-1")
            |> limit(n: 1)
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("sqlite").ok());
    ASSERT_TRUE(env.lookup("data").ok());
    ASSERT_EQ(Value::Type::Table, env.lookup("data")->type());
    EXPECT_FALSE(env.lookup("data")->as_table().materialized);
    auto materialized_or = execution::MaterializeValue(*env.lookup("data"));
    ASSERT_TRUE(materialized_or.ok()) << materialized_or.status();
    const auto& table = materialized_or->as_table();
    ASSERT_EQ(1, table.rows.size());
    ASSERT_NE(nullptr, table.rows[0]);
    EXPECT_EQ("\"edge-1\"", table.rows[0]->lookup("host")->string());
    EXPECT_EQ("71.5", table.rows[0]->lookup("usage")->string());
    EXPECT_EQ("\"west\"", table.rows[0]->lookup("region")->string());
}

TEST(RuntimeExecTest, SqliteFromAttachesSourceScanPlan) {
    auto file = ParseFile(R"(
        import "sqlite"

        data = sqlite.from(
    path: "cpp/pl/flux/examples/cross_source/metrics.db",
            table: "cpu",
        )
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    const auto data_or = env.lookup("data");
    ASSERT_TRUE(data_or.ok()) << data_or.status();
    ASSERT_EQ(Value::Type::Table, data_or->type());
    ASSERT_NE(nullptr, data_or->as_table().plan);
    EXPECT_FALSE(data_or->as_table().materialized);
    EXPECT_TRUE(data_or->as_table().rows.empty());
    EXPECT_EQ(plan::PlanNodeKind::SourceScan, data_or->as_table().plan->kind);
    EXPECT_EQ("sqlite", data_or->as_table().plan->source_scan().source);
    EXPECT_EQ("sqlite", data_or->as_table().plan->source_scan().driver);
    EXPECT_EQ("cpu", data_or->as_table().plan->source_scan().table);
}

TEST(RuntimeExecTest, SqlitePipelineAppendsLogicalPlanNodesWhileExecutingEagerly) {
    auto file = ParseFile(R"(
        import "sqlite"
        builtin range : (<-tables: stream[A], start: time, stop: time) => stream[A]
        builtin filter : (<-tables: stream[A], fn: (r: A) => bool) => stream[A]
        builtin keep : (<-tables: stream[A], columns: [string]) => stream[A]
        builtin sort : (<-tables: stream[A], columns: [string], desc: bool) => stream[A]
        builtin limit : (<-tables: stream[A], n: int) => stream[A]

        data = sqlite.from(
    path: "cpp/pl/flux/examples/cross_source/metrics.db",
            table: "cpu",
        )
            |> range(start: 2024-07-01T10:00:30Z, stop: 2024-07-01T10:04:00Z)
            |> filter(fn: (r) => r.host == "edge-1" and r.usage > 80.0)
            |> keep(columns: ["_time", "host", "usage"])
            |> sort(columns: ["usage"], desc: true)
            |> limit(n: 1)
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    const auto data_or = env.lookup("data");
    ASSERT_TRUE(data_or.ok()) << data_or.status();
    ASSERT_EQ(Value::Type::Table, data_or->type());
    EXPECT_FALSE(data_or->as_table().materialized);
    auto materialized_or = execution::MaterializeValue(*data_or);
    ASSERT_TRUE(materialized_or.ok()) << materialized_or.status();
    const auto& table = materialized_or->as_table();
    ASSERT_EQ(1, table.rows.size());
    EXPECT_EQ("93.25", table.rows[0]->lookup("usage")->string());

    auto node = data_or->as_table().plan;
    ASSERT_NE(nullptr, node);
    EXPECT_EQ(plan::PlanNodeKind::Limit, node->kind);
    ASSERT_EQ(1, node->inputs.size());
    node = node->inputs[0];
    ASSERT_NE(nullptr, node);
    EXPECT_EQ(plan::PlanNodeKind::Sort, node->kind);
    ASSERT_EQ(1, node->inputs.size());
    node = node->inputs[0];
    ASSERT_NE(nullptr, node);
    EXPECT_EQ(plan::PlanNodeKind::Project, node->kind);
    ASSERT_EQ(1, node->inputs.size());
    node = node->inputs[0];
    ASSERT_NE(nullptr, node);
    EXPECT_EQ(plan::PlanNodeKind::Filter, node->kind);
    ASSERT_EQ(2, node->filter().predicates.size());
    EXPECT_EQ("host", node->filter().predicates[0].column);
    EXPECT_EQ(plan::PredicateOp::Eq, node->filter().predicates[0].op);
    EXPECT_EQ("edge-1", node->filter().predicates[0].literal.string_value);
    EXPECT_EQ("usage", node->filter().predicates[1].column);
    EXPECT_EQ(plan::PredicateOp::Gt, node->filter().predicates[1].op);
    EXPECT_EQ(80.0, node->filter().predicates[1].literal.float_value);
    ASSERT_EQ(1, node->inputs.size());
    node = node->inputs[0];
    ASSERT_NE(nullptr, node);
    EXPECT_EQ(plan::PlanNodeKind::Range, node->kind);
    ASSERT_EQ(1, node->inputs.size());
    node = node->inputs[0];
    ASSERT_NE(nullptr, node);
    EXPECT_EQ(plan::PlanNodeKind::SourceScan, node->kind);
    EXPECT_EQ("sqlite", node->source_scan().driver);
}

TEST(RuntimeExecTest, MySQLPipelineExecutesThroughRuntimeAndPushdown) {
    auto dsn = mysql_test_dsn();
    if (!dsn.has_value()) {
        GTEST_SKIP() << "set FLUX_MYSQL_TEST_DSN and import "
                        "cpp/pl/flux/examples/cross_source/mysql_metrics.sql to run MySQL "
                        "runtime integration tests";
    }
    auto file = ParseFile(R"(
        import "mysql"
        builtin range : (<-tables: stream[A], start: time, stop: time) => stream[A]
        builtin filter : (<-tables: stream[A], fn: (r: A) => bool) => stream[A]
        builtin keep : (<-tables: stream[A], columns: [string]) => stream[A]
        builtin sort : (<-tables: stream[A], columns: [string], desc: bool) => stream[A]
        builtin limit : (<-tables: stream[A], n: int) => stream[A]

        data = mysql.from(
            dsn: ")" + *dsn +
                          R"(",
            table: "cpu",
        )
            |> range(start: 2024-07-01T10:00:30Z, stop: 2024-07-01T10:04:00Z)
            |> filter(fn: (r) => r.host == "edge-1")
            |> keep(columns: ["host", "usage"])
            |> sort(columns: ["usage"], desc: true)
            |> limit(n: 1)
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("mysql").ok());
    const auto data_or = env.lookup("data");
    ASSERT_TRUE(data_or.ok()) << data_or.status();
    ASSERT_EQ(Value::Type::Table, data_or->type());
    EXPECT_FALSE(data_or->as_table().materialized);
    auto materialized_or = execution::MaterializeValue(*data_or);
    ASSERT_TRUE(materialized_or.ok()) << materialized_or.status();
    const auto& table = materialized_or->as_table();
    ASSERT_EQ(1, table.rows.size());
    ASSERT_NE(nullptr, table.rows[0]);
    EXPECT_EQ("\"edge-1\"", table.rows[0]->lookup("host")->string());
    EXPECT_EQ("93.25", table.rows[0]->lookup("usage")->string());
    EXPECT_EQ(nullptr, table.rows[0]->lookup("_time"));

    auto node = data_or->as_table().plan;
    ASSERT_NE(nullptr, node);
    EXPECT_EQ(plan::PlanNodeKind::Limit, node->kind);
    ASSERT_EQ(1, node->inputs.size());
    node = node->inputs[0];
    ASSERT_NE(nullptr, node);
    EXPECT_EQ(plan::PlanNodeKind::Sort, node->kind);
    ASSERT_EQ(1, node->inputs.size());
    node = node->inputs[0];
    ASSERT_NE(nullptr, node);
    EXPECT_EQ(plan::PlanNodeKind::Project, node->kind);
    ASSERT_EQ(1, node->inputs.size());
    node = node->inputs[0];
    ASSERT_NE(nullptr, node);
    EXPECT_EQ(plan::PlanNodeKind::Filter, node->kind);
    ASSERT_EQ(1, node->inputs.size());
    node = node->inputs[0];
    ASSERT_NE(nullptr, node);
    EXPECT_EQ(plan::PlanNodeKind::Range, node->kind);
    ASSERT_EQ(1, node->inputs.size());
    node = node->inputs[0];
    ASSERT_NE(nullptr, node);
    EXPECT_EQ(plan::PlanNodeKind::SourceScan, node->kind);
    EXPECT_EQ("mysql", node->source_scan().driver);
}

TEST(RuntimeExecTest, ExplainFormatsLogicalPlan) {
    auto file = ParseFile(R"(
        import "sqlite"
        builtin filter : (<-tables: stream[A], fn: (r: A) => bool) => stream[A]
        builtin limit : (<-tables: stream[A], n: int) => stream[A]
        builtin explain : (<-tables: stream[A]) => string

        data = sqlite.from(
    path: "cpp/pl/flux/examples/cross_source/metrics.db",
            table: "cpu",
        )
            |> filter(fn: (r) => r.host == "edge-1")
            |> limit(n: 1)

        plan = data |> explain()
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    const auto plan_or = env.lookup("plan");
    ASSERT_TRUE(plan_or.ok()) << plan_or.status();
    ASSERT_EQ(Value::Type::String, plan_or->type());
    EXPECT_EQ(
        "Limit [sqlite pushdown]\n"
        "`- Filter [sqlite pushdown: host == \"edge-1\"]\n"
        "   `- SourceScan [sqlite scan](source=\"sqlite\", driver=\"sqlite\", table=\"cpu\")\n"
        "SourcePushdown(request: projection=[*], predicates=[host == \"edge-1\"], limit=1)\n",
        plan_or->as_string());
}

TEST(RuntimeExecTest, ExplainFormatsPhysicalPlan) {
    auto file = ParseFile(R"(
        import "sqlite"
        builtin filter : (<-tables: stream[A], fn: (r: A) => bool) => stream[A]
        builtin limit : (<-tables: stream[A], n: int) => stream[A]
        builtin explain : (<-tables: stream[A], ?physical: bool) => string

        data = sqlite.from(
            path: "cpp/pl/flux/examples/cross_source/metrics.db",
            table: "cpu",
        )
            |> filter(fn: (r) => r.host == "edge-1")
            |> limit(n: 1)

        plan = data |> explain(physical: true)
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    const auto plan_or = env.lookup("plan");
    ASSERT_TRUE(plan_or.ok()) << plan_or.status();
    ASSERT_EQ(Value::Type::String, plan_or->type());
    EXPECT_NE(std::string::npos, plan_or->as_string().find("PhysicalPlan\n"));
    EXPECT_NE(std::string::npos, plan_or->as_string().find("OutputSink [eager](name=\"output\""));
    EXPECT_NE(std::string::npos, plan_or->as_string().find("operator=\"OutputOperator\""));
    EXPECT_NE(std::string::npos, plan_or->as_string().find("ConnectorScan [lazy]"));
    EXPECT_NE(std::string::npos, plan_or->as_string().find("operator=\"ConnectorScanOperator\""));
    EXPECT_NE(std::string::npos, plan_or->as_string().find("table=\"cpu\""));
    EXPECT_NE(std::string::npos, plan_or->as_string().find("handle=\"sqlite:sqlite:cpu\""));
    EXPECT_NE(std::string::npos, plan_or->as_string().find("splits=1"));
    EXPECT_NE(std::string::npos,
              plan_or->as_string().find("logical_prefix=[Limit, Filter, SourceScan]"));
    EXPECT_NE(std::string::npos, plan_or->as_string().find("rbo=[PushLimitIntoConnectorScan, "
                                                           "PushPredicateIntoConnectorScan]"));
    EXPECT_NE(std::string::npos, plan_or->as_string().find("cbo=\"chosen\""));
    EXPECT_NE(std::string::npos, plan_or->as_string().find("cost={"));
}

TEST(RuntimeExecTest, ExplainFormatsOptimizedLogicalPlan) {
    auto file = ParseFile(R"(
        import "sqlite"
        builtin filter : (<-tables: stream[A], fn: (r: A) => bool) => stream[A]
        builtin limit : (<-tables: stream[A], n: int) => stream[A]
        builtin explain : (<-tables: stream[A], ?optimized: bool) => string

        data = sqlite.from(
            path: "cpp/pl/flux/examples/cross_source/metrics.db",
            table: "cpu",
        )
            |> filter(fn: (r) => r.host == "edge-1")
            |> limit(n: 1)

        plan = data |> explain(optimized: true)
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    const auto plan_or = env.lookup("plan");
    ASSERT_TRUE(plan_or.ok()) << plan_or.status();
    ASSERT_EQ(Value::Type::String, plan_or->type());
    EXPECT_EQ("OptimizedLogicalPlan\n"
              "Limit [sqlite pushdown]\n"
              "`- Filter [sqlite pushdown: host == \"edge-1\"]\n"
              "   `- SourceScan [sqlite scan](source=\"sqlite\", driver=\"sqlite\", "
              "table=\"cpu\")\n"
              "RBO(rules=[PushLimitIntoConnectorScan, PushPredicateIntoConnectorScan])\n"
              "SourcePushdown(request: projection=[*], predicates=[host == \"edge-1\"], "
              "limit=1)\n",
              plan_or->as_string());
}

TEST(RuntimeExecTest, ExplainDoesNotMaterializeLazySqliteSource) {
    auto file = ParseFile(R"(
        import "sqlite"
        builtin limit : (<-tables: stream[A], n: int) => stream[A]
        builtin explain : (<-tables: stream[A]) => string

        data = sqlite.from(
            path: "/tmp/flux-missing-lazy-source.db",
            table: "cpu",
        )
            |> limit(n: 1)

        plan = data |> explain()
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    const auto data_or = env.lookup("data");
    ASSERT_TRUE(data_or.ok()) << data_or.status();
    ASSERT_EQ(Value::Type::Table, data_or->type());
    EXPECT_FALSE(data_or->as_table().materialized);
    EXPECT_TRUE(data_or->as_table().rows.empty());

    const auto plan_or = env.lookup("plan");
    ASSERT_TRUE(plan_or.ok()) << plan_or.status();
    ASSERT_EQ(Value::Type::String, plan_or->type());
    EXPECT_EQ("Limit [sqlite pushdown]\n"
              "`- SourceScan [sqlite scan](source=\"sqlite\", driver=\"sqlite\", table=\"cpu\")\n",
              plan_or->as_string());

    auto materialized_or = execution::MaterializeValue(*data_or);
    ASSERT_FALSE(materialized_or.ok());
}

TEST(RuntimeExecTest, PhysicalExecutionFallsBackToMemoryOperatorAfterConnectorScan) {
    auto file = ParseFile(R"(
        import "sqlite"
        builtin group : (<-tables: stream[A], columns: [string]) => stream[A]
        builtin explain : (<-tables: stream[A], ?physical: bool) => string

        data = sqlite.from(
            path: "cpp/pl/flux/examples/cross_source/metrics.db",
            table: "cpu",
        )
            |> group(columns: ["host"])

        plan = data |> explain(physical: true)
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    const auto data_or = env.lookup("data");
    ASSERT_TRUE(data_or.ok()) << data_or.status();
    ASSERT_EQ(Value::Type::Table, data_or->type());
    EXPECT_FALSE(data_or->as_table().materialized);

    auto materialized_or = execution::MaterializeValue(*data_or);
    ASSERT_TRUE(materialized_or.ok()) << materialized_or.status();
    ASSERT_EQ(3, materialized_or->as_table().table_count());
    EXPECT_TRUE(materialized_or->as_table().materialized);

    const auto plan_or = env.lookup("plan");
    ASSERT_TRUE(plan_or.ok()) << plan_or.status();
    ASSERT_EQ(Value::Type::String, plan_or->type());
    EXPECT_NE(std::string::npos, plan_or->as_string().find("OutputSink [eager](name=\"output\""));
    EXPECT_NE(std::string::npos,
              plan_or->as_string().find("MemoryOperator [eager](name=\"Group\""));
    EXPECT_NE(std::string::npos, plan_or->as_string().find("operator=\"GroupOperator\""));
    EXPECT_NE(std::string::npos, plan_or->as_string().find("ConnectorScan [lazy]"));
    EXPECT_EQ(std::string::npos, plan_or->as_string().find("rbo=[PushAggregateIntoConnectorScan]"));
}

TEST(RuntimeExecTest, PhysicalExecutorRunsMemorySuffixAfterConnectorScan) {
    std::vector<plan::PredicateSpec> predicates = {
        {.op = plan::PredicateOp::Eq,
         .column = "region",
         .literal = {.kind = plan::PredicateLiteralKind::String, .string_value = "west"}},
    };
    auto plan = plan::MakeLimit(
        plan::MakeSort(
            plan::MakeRename(
                plan::MakeProject(plan::MakeFilter(plan::MakeGroup(SqliteCpuScanPlan(), {"host"}),
                                                   std::move(predicates)),
                                  {"host", "usage"}),
                {{"usage", "value"}}),
            {{.column = "value", .desc = true}}),
        1, 0);

    auto result_or = execution::PhysicalExecutor().Execute(plan);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_EQ(Value::Type::Table, result_or->type());
    const auto& table = result_or->as_table();
    EXPECT_TRUE(table.materialized);
    ASSERT_EQ(1, table.rows.size());
    ASSERT_NE(nullptr, table.rows[0]);
    EXPECT_EQ("\"edge-1\"", table.rows[0]->lookup("host")->string());
    EXPECT_EQ("93.25", table.rows[0]->lookup("value")->string());
    EXPECT_EQ(nullptr, table.rows[0]->lookup("usage"));
}

TEST(RuntimeExecTest, PhysicalExecutorRunsConnectorScanThroughEmptyPageSource) {
    auto plan = plan::MakeLimit(SqliteCpuScanPlan(), 0, 0);

    auto result_or = execution::PhysicalExecutor().Execute(plan);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_EQ(Value::Type::Table, result_or->type());
    const auto& table = result_or->as_table();
    EXPECT_TRUE(table.materialized);
    EXPECT_TRUE(table.rows.empty());
}

TEST(RuntimeExecTest, PhysicalPlannerBuildsOperatorPipeline) {
    std::vector<plan::PredicateSpec> predicates = {
        {.op = plan::PredicateOp::Eq,
         .column = "region",
         .literal = {.kind = plan::PredicateLiteralKind::String, .string_value = "west"}},
    };
    auto plan = plan::MakeLimit(
        plan::MakeFilter(plan::MakeGroup(SqliteCpuScanPlan(), {"host"}), std::move(predicates)), 1,
        0);

    auto task_or = execution::PhysicalPlanner().Plan(plan);

    ASSERT_TRUE(task_or.ok()) << task_or.status();
    ASSERT_EQ(1, task_or->pipelines.size());
    EXPECT_EQ("main", task_or->pipelines[0].name);
    EXPECT_EQ((std::vector<std::string>{
                  "ConnectorScanOperator",
                  "GroupOperator",
                  "FilterOperator",
                  "LimitOperator",
                  "OutputOperator",
              }),
              task_or->pipelines[0].operators);
}

TEST(RuntimeExecTest, PhysicalPlannerPreservesMaterializeOperatorInPipeline) {
    auto plan = plan::MakeAggregate(
        plan::MakeMaterializeBarrier(plan::MakeGroup(SqliteCpuScanPlan(), {"host"}),
                                     "unsupported lazy builtin", "test"),
        plan::AggregateFunction::Mean, "usage");

    auto task_or = execution::PhysicalPlanner().Plan(plan);

    ASSERT_TRUE(task_or.ok()) << task_or.status();
    ASSERT_EQ(1, task_or->pipelines.size());
    EXPECT_EQ((std::vector<std::string>{
                  "ConnectorScanOperator",
                  "GroupOperator",
                  "MaterializeOperator",
                  "AggregateOperator",
                  "OutputOperator",
              }),
              task_or->pipelines[0].operators);
}

TEST(RuntimeExecTest, PhysicalExecutorRunsMemoryAggregateAcrossMaterializeBarrier) {
    auto plan = plan::MakeAggregate(
        plan::MakeMaterializeBarrier(plan::MakeGroup(SqliteCpuScanPlan(), {"host"}),
                                     "unsupported lazy builtin", "test"),
        plan::AggregateFunction::Mean, "usage");

    auto result_or = execution::PhysicalExecutor().Execute(plan);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_EQ(Value::Type::Table, result_or->type());
    const auto& table = result_or->as_table();
    EXPECT_TRUE(table.materialized);
    ASSERT_EQ(3, table.rows.size());
    ASSERT_NE(nullptr, table.rows[0]);
    EXPECT_EQ("\"edge-1\"", table.rows[0]->lookup("host")->string());
    EXPECT_EQ("82.375", table.rows[0]->lookup("usage")->string());
    ASSERT_NE(nullptr, table.rows[1]);
    EXPECT_EQ("\"edge-2\"", table.rows[1]->lookup("host")->string());
    EXPECT_EQ("88", table.rows[1]->lookup("usage")->string());
    ASSERT_NE(nullptr, table.rows[2]);
    EXPECT_EQ("\"edge-3\"", table.rows[2]->lookup("host")->string());
    EXPECT_EQ("64.25", table.rows[2]->lookup("usage")->string());
}

TEST(RuntimeExecTest, PhysicalExecutorRunsMemoryDistinctAfterConnectorScan) {
    auto plan = plan::MakeDistinct(plan::MakeGroup(SqliteCpuScanPlan(), {"region"}), "host");

    auto result_or = execution::PhysicalExecutor().Execute(plan);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_EQ(Value::Type::Table, result_or->type());
    const auto& table = result_or->as_table();
    EXPECT_TRUE(table.materialized);
    ASSERT_EQ(3, table.rows.size());
    ASSERT_NE(nullptr, table.rows[0]);
    EXPECT_EQ("\"edge-1\"", table.rows[0]->lookup("host")->string());
    ASSERT_NE(nullptr, table.rows[1]);
    EXPECT_EQ("\"edge-2\"", table.rows[1]->lookup("host")->string());
    ASSERT_NE(nullptr, table.rows[2]);
    EXPECT_EQ("\"edge-3\"", table.rows[2]->lookup("host")->string());
}

TEST(RuntimeExecTest, PhysicalExecutorRunsLocalHashJoinAcrossConnectorInputs) {
    auto left = plan::MakeProject(SqliteCpuScanPlan(), {"host", "usage"});
    auto right = plan::MakeProject(SqliteCpuScanPlan(), {"host", "region"});
    auto join = plan::MakeJoin(std::move(left), std::move(right), {"host"});

    auto result_or = execution::PhysicalExecutor().Execute(join);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_EQ(Value::Type::Table, result_or->type());
    const auto& table = result_or->as_table();
    EXPECT_TRUE(table.materialized);
    ASSERT_EQ(6, table.rows.size());
    ASSERT_NE(nullptr, table.rows[0]);
    EXPECT_EQ("\"edge-1\"", table.rows[0]->lookup("host")->string());
    EXPECT_NE(nullptr, table.rows[0]->lookup("usage"));
    EXPECT_NE(nullptr, table.rows[0]->lookup("region"));
}

TEST(RuntimeExecTest, PhysicalPlannerBuildsMultiInputJoinPipeline) {
    auto join = plan::MakeJoin(plan::MakeProject(SqliteCpuScanPlan(), {"host", "usage"}),
                               plan::MakeProject(SqliteCpuScanPlan(), {"host", "region"}),
                               {"host"});

    auto task_or = execution::PhysicalPlanner().Plan(join);

    ASSERT_TRUE(task_or.ok()) << task_or.status();
    ASSERT_EQ(3, task_or->pipelines.size());
    EXPECT_EQ("join-left", task_or->pipelines[0].id);
    EXPECT_EQ("input", task_or->pipelines[0].role);
    EXPECT_EQ(nullptr, task_or->pipelines[0].root);
    EXPECT_EQ("join-right", task_or->pipelines[1].id);
    EXPECT_EQ("input", task_or->pipelines[1].role);
    EXPECT_EQ(nullptr, task_or->pipelines[1].root);
    EXPECT_EQ("main", task_or->pipelines[2].id);
    EXPECT_EQ((std::vector<std::string>{"join-left", "join-right"}),
              task_or->pipelines[2].dependencies);
    EXPECT_EQ((std::vector<std::string>{
                  "ConnectorScanOperator",
                  "ConnectorScanOperator",
                  "LocalHashJoinOperator",
                  "OutputOperator",
              }),
              task_or->pipelines[2].operators);
}

TEST(RuntimeExecTest, PhysicalExecutorRunsExchangeGatherAsPageBoundary) {
    auto plan = plan::MakeExchange(plan::MakeProject(SqliteCpuScanPlan(), {"host", "usage"}),
                                   plan::ExchangeKind::Gather);

    auto result_or = execution::PhysicalExecutor().Execute(plan);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_EQ(Value::Type::Table, result_or->type());
    const auto& table = result_or->as_table();
    EXPECT_TRUE(table.materialized);
    ASSERT_EQ(4, table.rows.size());
    ASSERT_EQ(1, table.tables.size());
    EXPECT_NE(nullptr, table.rows[0]->lookup("host"));
}

TEST(RuntimeExecTest, PhysicalExecutorRunsExchangeRepartitionByKeys) {
    auto plan = plan::MakeExchange(plan::MakeProject(SqliteCpuScanPlan(), {"host", "usage"}),
                                   plan::ExchangeKind::Repartition, {"host"});

    auto result_or = execution::PhysicalExecutor().Execute(plan);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_EQ(Value::Type::Table, result_or->type());
    const auto& table = result_or->as_table();
    EXPECT_TRUE(table.materialized);
    ASSERT_EQ(4, table.rows.size());
    ASSERT_EQ(3, table.tables.size());
    for (const auto& chunk : table.tables) {
        ASSERT_FALSE(chunk.rows.empty());
        ASSERT_NE(nullptr, chunk.rows[0]);
        const Value* host = chunk.rows[0]->lookup("host");
        ASSERT_NE(nullptr, host);
        for (const auto& row : chunk.rows) {
            ASSERT_NE(nullptr, row);
            const Value* row_host = row->lookup("host");
            ASSERT_NE(nullptr, row_host);
            EXPECT_EQ(host->string(), row_host->string());
        }
    }
}

TEST(RuntimeExecTest, PhysicalExplainShowsCboAlternatives) {
    auto plan = plan::MakeJoin(
        plan::MakeLimit(plan::MakeProject(SqliteCpuScanPlan(), {"host", "usage"}), 1, 0),
        plan::MakeProject(SqliteCpuScanPlan(), {"host", "region"}), {"host"});

    const auto physical = optimizer::FormatPhysicalPlan(plan);

    EXPECT_NE(std::string::npos, physical.find("alternatives=[local_hash_join_build_left*"));
    EXPECT_NE(std::string::npos, physical.find("local_hash_join_build_right"));
}

TEST(RuntimeExecTest, ContinuousSqliteFiltersAccumulatePushdownPredicates) {
    auto file = ParseFile(R"(
        import "sqlite"
        builtin filter : (<-tables: stream[A], fn: (r: A) => bool) => stream[A]
        builtin limit : (<-tables: stream[A], n: int) => stream[A]
        builtin explain : (<-tables: stream[A]) => string

        data = sqlite.from(
    path: "cpp/pl/flux/examples/cross_source/metrics.db",
            table: "cpu",
        )
            |> filter(fn: (r) => r.host == "edge-1")
            |> filter(fn: (r) => r.usage > 80.0)
            |> limit(n: 10)

        plan = data |> explain()
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    const auto data_or = env.lookup("data");
    ASSERT_TRUE(data_or.ok()) << data_or.status();
    ASSERT_EQ(Value::Type::Table, data_or->type());
    auto materialized_or = execution::MaterializeValue(*data_or);
    ASSERT_TRUE(materialized_or.ok()) << materialized_or.status();
    const auto& table = materialized_or->as_table();
    ASSERT_EQ(1, table.rows.size());
    EXPECT_EQ("\"edge-1\"", table.rows[0]->lookup("host")->string());
    EXPECT_EQ("93.25", table.rows[0]->lookup("usage")->string());

    const auto plan_or = env.lookup("plan");
    ASSERT_TRUE(plan_or.ok()) << plan_or.status();
    ASSERT_EQ(Value::Type::String, plan_or->type());
    EXPECT_EQ(
        "Limit [sqlite pushdown]\n"
        "`- Filter [sqlite pushdown: usage > 80]\n"
        "   `- Filter [sqlite pushdown: host == \"edge-1\"]\n"
        "      `- SourceScan [sqlite scan](source=\"sqlite\", driver=\"sqlite\", table=\"cpu\")\n"
        "SourcePushdown(request: projection=[*], predicates=[host == \"edge-1\", usage > "
        "80], limit=10)\n",
        plan_or->as_string());
}

TEST(RuntimeExecTest, SqliteDropColumnsPushesDownAsProjection) {
    auto file = ParseFile(R"(
        import "sqlite"
        builtin drop : (<-tables: stream[A], columns: [string]) => stream[B]
        builtin filter : (<-tables: stream[A], fn: (r: A) => bool) => stream[A]
        builtin limit : (<-tables: stream[A], n: int) => stream[A]
        builtin explain : (<-tables: stream[A]) => string

        data = sqlite.from(
    path: "cpp/pl/flux/examples/cross_source/metrics.db",
            table: "cpu",
        )
            |> drop(columns: ["region"])
            |> filter(fn: (r) => r.host == "edge-1")
            |> limit(n: 10)

        plan = data |> explain()
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    const auto data_or = env.lookup("data");
    ASSERT_TRUE(data_or.ok()) << data_or.status();
    ASSERT_EQ(Value::Type::Table, data_or->type());
    auto materialized_or = execution::MaterializeValue(*data_or);
    ASSERT_TRUE(materialized_or.ok()) << materialized_or.status();
    const auto& table = materialized_or->as_table();
    ASSERT_EQ(2, table.rows.size());
    EXPECT_EQ("\"edge-1\"", table.rows[0]->lookup("host")->string());
    EXPECT_EQ("71.5", table.rows[0]->lookup("usage")->string());
    EXPECT_EQ(nullptr, table.rows[0]->lookup("region"));
    EXPECT_EQ("\"edge-1\"", table.rows[1]->lookup("host")->string());
    EXPECT_EQ("93.25", table.rows[1]->lookup("usage")->string());
    EXPECT_EQ(nullptr, table.rows[1]->lookup("region"));

    const auto plan_or = env.lookup("plan");
    ASSERT_TRUE(plan_or.ok()) << plan_or.status();
    ASSERT_EQ(Value::Type::String, plan_or->type());
    EXPECT_EQ(
        "Limit [sqlite pushdown]\n"
        "`- Filter [sqlite pushdown: host == \"edge-1\"]\n"
        "   `- Project [sqlite pushdown]\n"
        "      `- SourceScan [sqlite scan](source=\"sqlite\", driver=\"sqlite\", table=\"cpu\")\n"
        "SourcePushdown(request: projection=[_time, host, usage], predicates=[host == "
        "\"edge-1\"], limit=10)\n",
        plan_or->as_string());
}

TEST(RuntimeExecTest, SqliteFilterAfterDropPreservesMissingColumnError) {
    auto file = ParseFile(R"(
        import "sqlite"
        builtin drop : (<-tables: stream[A], columns: [string]) => stream[B]
        builtin filter : (<-tables: stream[A], fn: (r: A) => bool) => stream[A]

        data = sqlite.from(
    path: "cpp/pl/flux/examples/cross_source/metrics.db",
            table: "cpu",
        )
            |> drop(columns: ["region"])
            |> filter(fn: (r) => r.region == "west")
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    const auto data_or = env.lookup("data");
    ASSERT_TRUE(data_or.ok()) << data_or.status();
    auto materialized_or = execution::MaterializeValue(*data_or);
    ASSERT_FALSE(materialized_or.ok());
    EXPECT_EQ(absl::StatusCode::kInvalidArgument, materialized_or.status().code());
    EXPECT_NE(std::string::npos,
              materialized_or.status().message().find("unavailable column: region"));
}

TEST(RuntimeExecTest, SqliteRenamePushesDownProjectionAliasAndColumnMapping) {
    auto file = ParseFile(R"(
        import "sqlite"
        builtin rename : (<-tables: stream[A], columns: A) => stream[B]
        builtin filter : (<-tables: stream[A], fn: (r: A) => bool) => stream[A]
        builtin keep : (<-tables: stream[A], columns: [string]) => stream[A]
        builtin sort : (<-tables: stream[A], columns: [string], desc: bool) => stream[A]
        builtin limit : (<-tables: stream[A], n: int) => stream[A]
        builtin explain : (<-tables: stream[A]) => string

        data = sqlite.from(
    path: "cpp/pl/flux/examples/cross_source/metrics.db",
            table: "cpu",
        )
            |> rename(columns: {usage: "value"})
            |> filter(fn: (r) => r.value > 80.0)
            |> keep(columns: ["host", "value"])
            |> sort(columns: ["value"], desc: true)
            |> limit(n: 1)

        plan = data |> explain()
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    const auto data_or = env.lookup("data");
    ASSERT_TRUE(data_or.ok()) << data_or.status();
    ASSERT_EQ(Value::Type::Table, data_or->type());
    auto materialized_or = execution::MaterializeValue(*data_or);
    ASSERT_TRUE(materialized_or.ok()) << materialized_or.status();
    const auto& table = materialized_or->as_table();
    ASSERT_EQ(1, table.rows.size());
    EXPECT_EQ("\"edge-1\"", table.rows[0]->lookup("host")->string());
    EXPECT_EQ("93.25", table.rows[0]->lookup("value")->string());
    EXPECT_EQ(nullptr, table.rows[0]->lookup("usage"));

    const auto plan_or = env.lookup("plan");
    ASSERT_TRUE(plan_or.ok()) << plan_or.status();
    ASSERT_EQ(Value::Type::String, plan_or->type());
    EXPECT_EQ("Limit [sqlite pushdown]\n"
              "`- Sort [sqlite pushdown]\n"
              "   `- Project [sqlite pushdown]\n"
              "      `- Filter [sqlite pushdown: value > 80]\n"
              "         `- Rename [sqlite pushdown]\n"
              "            `- SourceScan [sqlite scan](source=\"sqlite\", driver=\"sqlite\", "
              "table=\"cpu\")\n"
              "SourcePushdown(request: projection=[host, usage AS value], predicates=[usage > "
              "80], order_by=[usage DESC], limit=1)\n",
              plan_or->as_string());
}

TEST(RuntimeExecTest, SqliteFilterAfterRenamePreservesMissingOldColumnError) {
    auto file = ParseFile(R"(
        import "sqlite"
        builtin rename : (<-tables: stream[A], columns: A) => stream[B]
        builtin filter : (<-tables: stream[A], fn: (r: A) => bool) => stream[A]

        data = sqlite.from(
    path: "cpp/pl/flux/examples/cross_source/metrics.db",
            table: "cpu",
        )
            |> rename(columns: {usage: "value"})
            |> filter(fn: (r) => r.usage > 80.0)
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    const auto data_or = env.lookup("data");
    ASSERT_TRUE(data_or.ok()) << data_or.status();
    auto materialized_or = execution::MaterializeValue(*data_or);
    ASSERT_FALSE(materialized_or.ok());
    EXPECT_EQ(absl::StatusCode::kInvalidArgument, materialized_or.status().code());
    EXPECT_NE(std::string::npos,
              materialized_or.status().message().find("unavailable column: usage"));
}

TEST(RuntimeExecTest, SqliteGroupCountPushesDownAggregate) {
    auto file = ParseFile(R"(
        import "sqlite"
        builtin filter : (<-tables: stream[A], fn: (r: A) => bool) => stream[A]
        builtin group : (<-tables: stream[A], columns: [string]) => stream[A]
        builtin count : (<-tables: stream[A], ?column: string) => stream[A]
        builtin explain : (<-tables: stream[A]) => string

        data = sqlite.from(
    path: "cpp/pl/flux/examples/cross_source/metrics.db",
            table: "cpu",
        )
            |> filter(fn: (r) => r.region == "west")
            |> group(columns: ["host"])
            |> count(column: "usage")

        plan = data |> explain()
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    const auto data_or = env.lookup("data");
    ASSERT_TRUE(data_or.ok()) << data_or.status();
    ASSERT_EQ(Value::Type::Table, data_or->type());
    auto materialized_or = execution::MaterializeValue(*data_or);
    ASSERT_TRUE(materialized_or.ok()) << materialized_or.status();
    const auto& table = materialized_or->as_table();
    ASSERT_EQ(2, table.rows.size());
    EXPECT_EQ("\"edge-1\"", table.rows[0]->lookup("host")->string());
    EXPECT_EQ("2", table.rows[0]->lookup("usage")->string());
    EXPECT_NE(nullptr, table.rows[0]->lookup("_group"));
    EXPECT_EQ("\"edge-2\"", table.rows[1]->lookup("host")->string());
    EXPECT_EQ("1", table.rows[1]->lookup("usage")->string());

    const auto plan_or = env.lookup("plan");
    ASSERT_TRUE(plan_or.ok()) << plan_or.status();
    ASSERT_EQ(Value::Type::String, plan_or->type());
    EXPECT_EQ(
        "Aggregate [sqlite pushdown]\n"
        "`- Group [sqlite pushdown]\n"
        "   `- Filter [sqlite pushdown: region == \"west\"]\n"
        "      `- SourceScan [sqlite scan](source=\"sqlite\", driver=\"sqlite\", table=\"cpu\")\n"
        "SourcePushdown(request: projection=[*], predicates=[region == \"west\"], "
        "group_by=[host], aggregate=COUNT(usage))\n",
        plan_or->as_string());
}

TEST(RuntimeExecTest, SqliteGroupMeanAfterRenamePushesDownAggregateMapping) {
    auto file = ParseFile(R"(
        import "sqlite"
        builtin rename : (<-tables: stream[A], columns: A) => stream[B]
        builtin group : (<-tables: stream[A], columns: [string]) => stream[A]
        builtin mean : (<-tables: stream[A], ?column: string) => stream[A]

        data = sqlite.from(
    path: "cpp/pl/flux/examples/cross_source/metrics.db",
            table: "cpu",
        )
            |> rename(columns: {usage: "value"})
            |> group(columns: ["host"])
            |> mean(column: "value")
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    const auto data_or = env.lookup("data");
    ASSERT_TRUE(data_or.ok()) << data_or.status();
    ASSERT_EQ(Value::Type::Table, data_or->type());
    auto materialized_or = execution::MaterializeValue(*data_or);
    ASSERT_TRUE(materialized_or.ok()) << materialized_or.status();
    const auto& table = materialized_or->as_table();
    ASSERT_EQ(3, table.rows.size());
    EXPECT_EQ("\"edge-1\"", table.rows[0]->lookup("host")->string());
    EXPECT_EQ("82.375", table.rows[0]->lookup("value")->string());
    EXPECT_EQ("\"edge-2\"", table.rows[1]->lookup("host")->string());
    EXPECT_EQ("88", table.rows[1]->lookup("value")->string());
    EXPECT_EQ("\"edge-3\"", table.rows[2]->lookup("host")->string());
    EXPECT_EQ("64.25", table.rows[2]->lookup("value")->string());
}

TEST(RuntimeExecTest, SqliteDistinctAfterRenamePushesDownColumnMapping) {
    auto file = ParseFile(R"(
        import "sqlite"
        builtin rename : (<-tables: stream[A], columns: A) => stream[B]
        builtin distinct : (<-tables: stream[A], ?column: string) => stream[A]
        builtin keep : (<-tables: stream[A], columns: [string]) => stream[A]
        builtin sort : (<-tables: stream[A], columns: [string], desc: bool) => stream[A]
        builtin explain : (<-tables: stream[A]) => string

        data = sqlite.from(
    path: "cpp/pl/flux/examples/cross_source/metrics.db",
            table: "cpu",
        )
            |> rename(columns: {host: "service", usage: "value"})
            |> distinct(column: "service")
            |> keep(columns: ["service"])
            |> sort(columns: ["service"], desc: false)

        plan = data |> explain()
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    const auto data_or = env.lookup("data");
    ASSERT_TRUE(data_or.ok()) << data_or.status();
    ASSERT_EQ(Value::Type::Table, data_or->type());
    auto materialized_or = execution::MaterializeValue(*data_or);
    ASSERT_TRUE(materialized_or.ok()) << materialized_or.status();
    const auto& table = materialized_or->as_table();
    ASSERT_EQ(3, table.rows.size());
    EXPECT_EQ("\"edge-1\"", table.rows[0]->lookup("service")->string());
    EXPECT_EQ(nullptr, table.rows[0]->lookup("host"));
    EXPECT_EQ("\"edge-2\"", table.rows[1]->lookup("service")->string());
    EXPECT_EQ("\"edge-3\"", table.rows[2]->lookup("service")->string());

    const auto plan_or = env.lookup("plan");
    ASSERT_TRUE(plan_or.ok()) << plan_or.status();
    ASSERT_EQ(Value::Type::String, plan_or->type());
    EXPECT_EQ("Sort [sqlite pushdown]\n"
              "`- Project [sqlite pushdown]\n"
              "   `- Distinct [sqlite pushdown]\n"
              "      `- Rename [sqlite pushdown]\n"
              "         `- SourceScan [sqlite scan](source=\"sqlite\", driver=\"sqlite\", "
              "table=\"cpu\")\n"
              "SourcePushdown(request: projection=[host AS service], distinct=host, "
              "order_by=[host ASC])\n",
              plan_or->as_string());
}

TEST(RuntimeExecTest, ComplexSqliteFilterFallsBackToMaterializedExecution) {
    auto file = ParseFile(R"(
        import "sqlite"
        builtin filter : (<-tables: stream[A], fn: (r: A) => bool) => stream[A]
        builtin explain : (<-tables: stream[A]) => string

        data = sqlite.from(
    path: "cpp/pl/flux/examples/cross_source/metrics.db",
            table: "cpu",
        )
            |> filter(fn: (r) => r.host == "edge-1" or r.usage > 80.0)

        plan = data |> explain()
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    const auto data_or = env.lookup("data");
    ASSERT_TRUE(data_or.ok()) << data_or.status();
    ASSERT_EQ(Value::Type::Table, data_or->type());
    auto materialized_or = execution::MaterializeValue(*data_or);
    ASSERT_TRUE(materialized_or.ok()) << materialized_or.status();
    const auto& table = materialized_or->as_table();
    ASSERT_EQ(3, table.rows.size());
    EXPECT_EQ("\"edge-1\"", table.rows[0]->lookup("host")->string());

    const auto plan_or = env.lookup("plan");
    ASSERT_TRUE(plan_or.ok()) << plan_or.status();
    ASSERT_EQ(Value::Type::String, plan_or->type());
    EXPECT_EQ(
        "Materialize [barrier: unsupported lazy builtin](reason=\"unsupported lazy builtin\", "
        "builtin=\"filter\")\n"
        "`- SourceScan [sqlite scan](source=\"sqlite\", driver=\"sqlite\", table=\"cpu\")\n",
        plan_or->as_string());
}

TEST(RuntimeExecTest, UnsupportedLazyBuiltinAddsMaterializationBarrier) {
    auto file = ParseFile(R"(
        import "sqlite"
        builtin map : (<-tables: stream[A], fn: (r: A) => B) => stream[B]
        builtin limit : (<-tables: stream[A], n: int) => stream[A]
        builtin explain : (<-tables: stream[A]) => string

        data = sqlite.from(
    path: "cpp/pl/flux/examples/cross_source/metrics.db",
            table: "cpu",
        )
            |> map(fn: (r) => ({host: r.host, score: r.usage * 100.0}))
            |> limit(n: 1)

        plan = data |> explain()
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    const auto plan_or = env.lookup("plan");
    ASSERT_TRUE(plan_or.ok()) << plan_or.status();
    ASSERT_EQ(Value::Type::String, plan_or->type());
    EXPECT_EQ(
        "Limit [memory]\n"
        "`- Materialize [barrier: unsupported lazy builtin](reason=\"unsupported lazy builtin\", "
        "builtin=\"map\")\n"
        "   `- SourceScan [sqlite scan](source=\"sqlite\", driver=\"sqlite\", table=\"cpu\")\n",
        plan_or->as_string());
}

TEST(RuntimeExecTest, SqliteFromRejectsQueryArgument) {
    auto file = ParseFile(R"(
        import "sqlite"
        data = sqlite.from(path: ":memory:", query: "select 1")
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_FALSE(result_or.ok());
    EXPECT_EQ(absl::StatusCode::kInvalidArgument, result_or.status().code());
    EXPECT_NE(std::string::npos, result_or.status().message().find("does not accept `query`"));
}

TEST(RuntimeExecTest, ImportsMysqlPackage) {
    auto file = ParseFile(R"(
        import "mysql"
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("mysql").ok());
    EXPECT_EQ("{path: \"mysql\", from: <builtin mysql.from>}", env.lookup("mysql")->string());
}

TEST(RuntimeExecTest, MysqlFromRejectsRawQueryArgument) {
    auto file = ParseFile(R"(
        import "mysql"
        data = mysql.from(dsn: "mysql://metrics", query: "select 1")
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_FALSE(result_or.ok());
    EXPECT_EQ(absl::StatusCode::kInvalidArgument, result_or.status().code());
    EXPECT_NE(std::string::npos, result_or.status().message().find("does not accept `query`"));
}

TEST(RuntimeExecTest, MysqlFromRequiresConnectionShape) {
    auto file = ParseFile(R"(
        import "mysql"
        data = mysql.from(table: "cpu")
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_FALSE(result_or.ok());
    EXPECT_EQ(absl::StatusCode::kInvalidArgument, result_or.status().code());
    EXPECT_NE(std::string::npos, result_or.status().message().find("requires `host`"));
}

TEST(RuntimeExecTest, MysqlFromRejectsMixedDsnAndHostConnectionShape) {
    auto file = ParseFile(R"(
        import "mysql"
        data = mysql.from(
            dsn: "user:pass@tcp(127.0.0.1:3306)/metrics",
            host: "127.0.0.1",
            table: "cpu",
        )
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_FALSE(result_or.ok());
    EXPECT_EQ(absl::StatusCode::kInvalidArgument, result_or.status().code());
    EXPECT_NE(std::string::npos,
              result_or.status().message().find("accepts either `dsn` or host/user"));
}

TEST(RuntimeExecTest, DeclaresUnknownBuiltinAsPlaceholderFunction) {
    auto file = ParseFile(R"(
        builtin mystery : (a: int) => int
        result = mystery(1)
    )");
    ASSERT_NE(file, nullptr);
    ASSERT_EQ(2, file->body.size());

    Environment env;
    auto builtin_or = StatementExecutor::Execute(*file->body[0], env);
    ASSERT_TRUE(builtin_or.ok()) << builtin_or.status();
    ASSERT_TRUE(env.lookup("mystery").ok());
    EXPECT_EQ(Value::Type::Function, env.lookup("mystery")->type());

    auto result = StatementExecutor::Execute(*file->body[1], env);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(absl::StatusCode::kUnimplemented, result.status().code());
}

TEST(RuntimeExecTest, DeclaresTopLevelFromAsPlaceholderFunctionOnly) {
    auto file = ParseFile(R"(
        builtin from : (bucket: string) => stream[A]
        result = from(bucket: "telegraf")
    )");
    ASSERT_NE(file, nullptr);
    ASSERT_EQ(2, file->body.size());

    Environment env;
    BuiltinRegistry::Install(env);
    EXPECT_FALSE(env.lookup("from").ok());

    auto builtin_or = StatementExecutor::Execute(*file->body[0], env);
    ASSERT_TRUE(builtin_or.ok()) << builtin_or.status();
    ASSERT_TRUE(env.lookup("from").ok());
    EXPECT_EQ(Value::Type::Function, env.lookup("from")->type());

    auto result = StatementExecutor::Execute(*file->body[1], env);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(absl::StatusCode::kUnimplemented, result.status().code());
}

TEST(RuntimeExecTest, ExecutesTopLevelFileStatementsInSharedEnvironment) {
    auto file = ParseFile(R"(
        package metrics
        import "array"
        import regexp "regexp"
        option task = {name: "cpu"}
        base = 2
        result = base |> ((<-value, ?inc=1) => value + inc)(inc: 3)
        result
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    EXPECT_EQ(ExecutionResult::Type::Normal, result_or->last.type);
    EXPECT_EQ("5", result_or->last.value.string());
    ASSERT_EQ(4, result_or->results.size());
    EXPECT_EQ("option.task", result_or->results[0].name);
    EXPECT_EQ("{name: \"cpu\"}", result_or->results[0].value.string());
    EXPECT_EQ("base", result_or->results[1].name);
    EXPECT_EQ("2", result_or->results[1].value.string());
    EXPECT_EQ("result", result_or->results[2].name);
    EXPECT_EQ("5", result_or->results[2].value.string());
    EXPECT_EQ("_result", result_or->results[3].name);
    EXPECT_EQ("5", result_or->results[3].value.string());
    EXPECT_EQ("metrics", result_or->package_name);
    ASSERT_EQ(2, result_or->imports.size());
    EXPECT_EQ("array", result_or->imports[0]);
    EXPECT_EQ("regexp", result_or->imports[1]);
    ASSERT_TRUE(env.lookup("base").ok());
    EXPECT_EQ("2", env.lookup("base")->string());
    ASSERT_TRUE(env.lookup("result").ok());
    EXPECT_EQ("5", env.lookup("result")->string());
    ASSERT_TRUE(env.lookup_option("task").ok());
    EXPECT_EQ("{name: \"cpu\"}", env.lookup_option("task")->string());
    ASSERT_TRUE(env.lookup_option("__flux.package").ok());
    EXPECT_EQ("\"metrics\"", env.lookup_option("__flux.package")->string());
    ASSERT_TRUE(env.lookup("array").ok());
    EXPECT_EQ("{path: \"array\", from: <builtin array.from>, concat: <builtin array.concat>, "
              "filter: <builtin array.filter>, map: <builtin array.map>, "
              "contains: <builtin array.contains>, reduce: <builtin array.reduce>, "
              "any: <builtin array.any>, all: <builtin array.all>}",
              env.lookup("array")->string());
    ASSERT_TRUE(env.lookup("regexp").ok());
    EXPECT_EQ("{path: \"regexp\", compile: <builtin regexp.compile>, "
              "findString: <builtin regexp.findString>, "
              "matchRegexpString: <builtin regexp.matchRegexpString>, "
              "quoteMeta: <builtin regexp.quoteMeta>, alias: \"regexp\"}",
              env.lookup("regexp")->string());
}

TEST(RuntimeExecTest, ExecutesScalarStdlibPackageHelpers) {
    auto file = ParseFile(R"(
        import "date"
        import "math"
        import "regexp"
        import "strings"

        service = " api-01 "
            |> strings.trimSpace()
            |> strings.toUpper()
        matched = regexp.matchRegexpString(r: regexp.compile(v: "API-[0-9]+"), v: service)
        team = strings.joinStr(arr: strings.split(v: "payments.api.edge", t: "."), v: "/")
        host = regexp.findString(r: /edge-[0-9]+/, v: "host=edge-7")
        node = strings.replaceAll(v: service, t: "API", u: "SVC")
        score = math.pow(x: 2.0, y: 4.0) + math.abs(x: -3)
        start = date.truncate(t: 2024-06-01T09:37:10Z, unit: 1h)
        due = start |> date.add(d: 30m)
        opened = date.sub(d: 15m, from: due)
        hour = date.hour(t: due)
        weekday = "2024-06-01T09:01:10Z" |> date.weekDay()
        {
            service: service,
            team: team,
            host: host,
            node: node,
            matched: matched,
            score: score,
            start: start,
            due: due,
            opened: opened,
            hour: hour,
            weekday: weekday,
        }
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("service").ok());
    EXPECT_EQ("\"API-01\"", env.lookup("service")->string());
    ASSERT_TRUE(env.lookup("matched").ok());
    EXPECT_EQ("true", env.lookup("matched")->string());
    ASSERT_TRUE(env.lookup("score").ok());
    EXPECT_EQ("19", env.lookup("score")->string());
    EXPECT_EQ("{service: \"API-01\", team: \"payments/api/edge\", host: \"edge-7\", node: "
              "\"SVC-01\", matched: true, score: 19, start: 2024-06-01T09:00:00Z, due: "
              "2024-06-01T09:30:00Z, opened: 2024-06-01T09:15:00Z, hour: 9, weekday: 6}",
              result_or->last.value.string());
}

TEST(RuntimeExecTest, ImportsUnknownPackageAsMetadataOnlyObject) {
    auto file = ParseFile(R"(
        import custom "experimental/unknown"
        custom
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("custom").ok());
    EXPECT_EQ("{path: \"experimental/unknown\", alias: \"custom\"}",
              env.lookup("custom")->string());
    EXPECT_EQ("{path: \"experimental/unknown\", alias: \"custom\"}",
              result_or->last.value.string());
}

TEST(RuntimeExecTest, ImportsUnaliasedMultiSegmentPackageByLastPathSegment) {
    auto file = ParseFile(R"(
        import "experimental/unknown"
        unknown.path
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("unknown").ok());
    ASSERT_FALSE(env.lookup("experimental/unknown").ok());
    EXPECT_EQ("\"experimental/unknown\"", result_or->last.value.string());
}

TEST(RuntimeExecTest, ExecutesArrayFromImportedPackage) {
    auto file = ParseFile(R"(
        import "array"

        data = array.from(
            rows: [
                {_time: "2024-01-01T00:00:00Z", host: "edge-1", _value: 70},
                {_time: "2024-01-01T00:01:00Z", host: "edge-2", _value: 91},
            ],
        )
        data
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("data").ok());
    ASSERT_EQ(Value::Type::Table, env.lookup("data")->type());
    EXPECT_EQ("array", env.lookup("data")->as_table().bucket);
    ASSERT_EQ(2, env.lookup("data")->as_table().rows.size());
    ASSERT_NE(nullptr, env.lookup("data")->as_table().rows[1]);
    EXPECT_EQ("\"edge-2\"", env.lookup("data")->as_table().rows[1]->lookup("host")->string());
    EXPECT_EQ("91", env.lookup("data")->as_table().rows[1]->lookup("_value")->string());
    ASSERT_EQ(Value::Type::Table, result_or->last.value.type());
}

TEST(RuntimeExecTest, TopLevelFromIsNotRuntimeBuiltin) {
    auto file = ParseFile(R"(
        data = from(bucket: "telegraf", rows: [{_value: 1}])
        data
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    BuiltinRegistry::Install(env);
    EXPECT_FALSE(env.lookup("from").ok());
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_FALSE(result_or.ok());
    EXPECT_EQ(absl::StatusCode::kNotFound, result_or.status().code());
}

TEST(RuntimeExecTest, ExecutesArrayPackageHelpers) {
    auto file = ParseFile(R"(
        import "array"

        rows = [1, 2, 3]
            |> array.concat(v: [4, 5])
            |> array.filter(fn: (x) => x >= 3)
            |> array.map(fn: (x) => ({host: "edge-${x}", _value: x * 10}))
        data = array.from(rows: rows)
        data
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("rows").ok());
    ASSERT_EQ(Value::Type::Array, env.lookup("rows")->type());
    ASSERT_EQ(3, env.lookup("rows")->as_array().elements.size());
    EXPECT_EQ("{host: \"edge-3\", _value: 30}",
              env.lookup("rows")->as_array().elements[0].string());
    ASSERT_TRUE(env.lookup("data").ok());
    ASSERT_EQ(Value::Type::Table, env.lookup("data")->type());
    ASSERT_EQ(3, env.lookup("data")->as_table().rows.size());
    EXPECT_EQ("\"edge-5\"", env.lookup("data")->as_table().rows[2]->lookup("host")->string());
    EXPECT_EQ("50", env.lookup("data")->as_table().rows[2]->lookup("_value")->string());
    ASSERT_EQ(Value::Type::Table, result_or->last.value.type());
}

TEST(RuntimeExecTest, ExecutesArrayContainsAndReduceHelpers) {
    auto file = ParseFile(R"(
        import "array"

        watchlist = [
            {host: "edge-1", owner: "ops"},
            {host: "edge-3", owner: "canary"},
        ]
        hosts = watchlist |> array.map(fn: (r) => r.host)
        hasCanary = hosts |> array.contains(value: "edge-3")
        summary = hosts
            |> array.reduce(
                identity: {count: 0, last: ""},
                fn: (host, accumulator) => ({
                    count: accumulator.count + 1,
                    last: host,
                }),
            )
        {hasCanary: hasCanary, summary: summary}
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("hasCanary").ok());
    ASSERT_EQ(Value::Type::Bool, env.lookup("hasCanary")->type());
    EXPECT_TRUE(env.lookup("hasCanary")->as_bool());
    ASSERT_TRUE(env.lookup("summary").ok());
    ASSERT_EQ(Value::Type::Object, env.lookup("summary")->type());
    EXPECT_EQ("2", env.lookup("summary")->as_object().lookup("count")->string());
    EXPECT_EQ("\"edge-3\"", env.lookup("summary")->as_object().lookup("last")->string());
    ASSERT_EQ(Value::Type::Object, result_or->last.value.type());
    EXPECT_EQ("{hasCanary: true, summary: {count: 2, last: \"edge-3\"}}",
              result_or->last.value.string());
}

TEST(RuntimeExecTest, ExecutesArrayAnyAndAllHelpers) {
    auto file = ParseFile(R"(
        import "array"

        loads = [71.0, 88.0, 64.0]
        hasCritical = loads |> array.any(fn: (x) => x >= 85.0)
        allHealthy = loads |> array.all(fn: (x) => x >= 60.0)
        {hasCritical: hasCritical, allHealthy: allHealthy}
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("hasCritical").ok());
    ASSERT_EQ(Value::Type::Bool, env.lookup("hasCritical")->type());
    EXPECT_TRUE(env.lookup("hasCritical")->as_bool());
    ASSERT_TRUE(env.lookup("allHealthy").ok());
    ASSERT_EQ(Value::Type::Bool, env.lookup("allHealthy")->type());
    EXPECT_TRUE(env.lookup("allHealthy")->as_bool());
    ASSERT_EQ(Value::Type::Object, result_or->last.value.type());
    EXPECT_EQ("{hasCritical: true, allHealthy: true}", result_or->last.value.string());
}

TEST(RuntimeExecTest, ResolvesOptionValuesInsideExpressionsAndBlockFunctions) {
    auto file = ParseFile(R"(
        option task = {name: "cpu-alert", every: 5m}
        option task.offset = 30s
        option task.owner = "ops"

        decorate = (r) => {
            level = if r._value >= 80.0 then "critical" else if r._value >= 70.0 then "warm" else "steady"
            return {
                task: task.name,
                owner: task.owner,
                every: task.every,
                offset: task.offset,
                host: r.host,
                level: level,
            }
        }

        result = decorate(r: {host: "edge-1", _value: 82.0})
        result
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_EQ(Value::Type::Object, result_or->last.value.type());
    ASSERT_TRUE(env.lookup("result").ok());
    ASSERT_EQ(Value::Type::Object, env.lookup("result")->type());
    const auto& result = env.lookup("result")->as_object();
    ASSERT_NE(nullptr, result.lookup("task"));
    ASSERT_NE(nullptr, result.lookup("owner"));
    ASSERT_NE(nullptr, result.lookup("every"));
    ASSERT_NE(nullptr, result.lookup("offset"));
    ASSERT_NE(nullptr, result.lookup("level"));
    EXPECT_EQ("\"cpu-alert\"", result.lookup("task")->string());
    EXPECT_EQ("\"ops\"", result.lookup("owner")->string());
    EXPECT_EQ("5m", result.lookup("every")->string());
    EXPECT_EQ("30s", result.lookup("offset")->string());
    EXPECT_EQ("\"critical\"", result.lookup("level")->string());
}

TEST(RuntimeExecTest, AppliesGlobalOptionLocationDuringFileExecution) {
    auto file = ParseFile(R"(
        import "array"

        option location = {zone: "UTC", offset: "-8h"}
        builtin aggregateWindow : (<-tables: stream[A], every: duration, fn: (values: [B]) => C) => stream[A]
        builtin sum : (values: [int]) => float

        result = array.from(
            bucket: "telegraf",
            rows: [
                {_time: "2024-01-01T07:30:00Z", _value: 10.0},
                {_time: "2024-01-01T08:30:00Z", _value: 30.0},
            ],
        )
            |> aggregateWindow(every: 1d, fn: sum)
        result
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("result").ok());
    ASSERT_EQ(Value::Type::Table, env.lookup("result")->type());
    ASSERT_EQ(2, env.lookup("result")->as_table().rows.size());
    EXPECT_EQ("10", env.lookup("result")->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("2023-12-31T08:00:00Z",
              env.lookup("result")->as_table().rows[0]->lookup("_start")->string());
    EXPECT_EQ("2024-01-01T08:00:00Z",
              env.lookup("result")->as_table().rows[0]->lookup("_stop")->string());
    EXPECT_EQ("30", env.lookup("result")->as_table().rows[1]->lookup("_value")->string());
    EXPECT_EQ("2024-01-01T08:00:00Z",
              env.lookup("result")->as_table().rows[1]->lookup("_start")->string());
    EXPECT_EQ("2024-01-02T08:00:00Z",
              env.lookup("result")->as_table().rows[1]->lookup("_stop")->string());
}

TEST(RuntimeExecTest, RejectsTopLevelReturnDuringFileExecution) {
    auto file = ParseFile(R"(
        value = 1
        return value
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(absl::StatusCode::kInvalidArgument, result.status().code());
}

TEST(RuntimeExecTest, ExecutesTestCasesDuringFileExecutionWithoutLeakingBindings) {
    auto file = ParseFile(R"(
        base = 10
        testcase checks {
            local = base + 5
            return local
        }
        base
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    EXPECT_EQ("10", result_or->last.value.string());
    ASSERT_EQ(3, result_or->results.size());
    EXPECT_EQ("base", result_or->results[0].name);
    EXPECT_EQ("10", result_or->results[0].value.string());
    EXPECT_EQ("testcase.checks", result_or->results[1].name);
    EXPECT_EQ("{name: \"checks\", success: true, value: 15}", result_or->results[1].value.string());
    EXPECT_EQ("_result", result_or->results[2].name);
    EXPECT_EQ("10", result_or->results[2].value.string());
    EXPECT_FALSE(env.lookup("local").ok());
    auto testcase_or = env.lookup_option("__flux.testcase.checks");
    ASSERT_TRUE(testcase_or.ok()) << testcase_or.status();
    EXPECT_EQ("{name: \"checks\", success: true, value: 15}", testcase_or->string());
}

TEST(RuntimeExecTest, TracksNamedResultsForAssignmentsAndExpressions) {
    auto file = ParseFile(R"(
        value = 42
        value
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_EQ(2, result_or->results.size());
    EXPECT_EQ("value", result_or->results[0].name);
    EXPECT_EQ("42", result_or->results[0].value.string());
    EXPECT_EQ("_result", result_or->results[1].name);
    EXPECT_EQ("42", result_or->results[1].value.string());
}

TEST(RuntimeExecTest, ExecutesInMemoryQueryPipelineFile) {
    auto file = ParseFile(R"(
        import "array"

        builtin range : (<-tables: stream[A], start: time, ?stop: time) => stream[A]
        builtin filter : (<-tables: stream[A], fn: (r: A) => bool) => stream[A]
        builtin map : (<-tables: stream[A], fn: (r: A) => B) => stream[B]

        result = array.from(
            bucket: "telegraf",
            rows: [
                {_time: "2024-01-01T00:00:00Z", _measurement: "cpu", _value: 95.0},
                {_time: "2024-01-02T00:00:00Z", _measurement: "cpu", _value: 70.0},
                {_time: "2024-01-03T00:00:00Z", _measurement: "mem", _value: 40.0},
            ],
        )
            |> range(start: 2024-01-01T00:00:00Z, stop: 2024-01-02T12:00:00Z)
            |> filter(fn: (r) => r._measurement == "cpu" and r._value > 80.0)
            |> map(fn: (r) => ({r with level: "critical"}))
        result
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_EQ(Value::Type::Table, result_or->last.value.type());
    ASSERT_TRUE(env.lookup("result").ok());
    ASSERT_EQ(Value::Type::Table, env.lookup("result")->type());
    EXPECT_EQ("telegraf", env.lookup("result")->as_table().bucket);
    ASSERT_EQ(1, env.lookup("result")->as_table().rows.size());
    ASSERT_NE(nullptr, env.lookup("result")->as_table().rows[0]);
    EXPECT_EQ("\"critical\"", env.lookup("result")->as_table().rows[0]->lookup("level")->string());
}

TEST(RuntimeExecTest, ExecutesReduceKeepDropAndLimitQueryFile) {
    auto file = ParseFile(R"(
        import "array"

        builtin limit : (<-tables: stream[A], n: int) => stream[A]
        builtin keep : (<-tables: stream[A], columns: [string]) => stream[A]
        builtin reduce : (<-tables: stream[A], identity: B, fn: (r: A, accumulator: B) => B) => stream[B]
        builtin drop : (<-tables: stream[A], columns: [string]) => stream[A]

        totals = array.from(
            bucket: "telegraf",
            rows: [
                {_measurement: "cpu", _value: 90.0, host: "a"},
                {_measurement: "cpu", _value: 60.0, host: "b"},
                {_measurement: "mem", _value: 10.0, host: "c"},
            ],
        )
            |> limit(n: 2)
            |> keep(columns: ["_value", "host"])
            |> reduce(
                identity: {count: 0, total: 0.0},
                fn: (r, accumulator) => ({
                    count: accumulator.count + 1,
                    total: accumulator.total + r._value,
                }),
            )
            |> drop(columns: ["count"])
        totals
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_EQ(Value::Type::Table, result_or->last.value.type());
    ASSERT_TRUE(env.lookup("totals").ok());
    ASSERT_EQ(1, env.lookup("totals")->as_table().rows.size());
    ASSERT_NE(nullptr, env.lookup("totals")->as_table().rows[0]);
    EXPECT_EQ(nullptr, env.lookup("totals")->as_table().rows[0]->lookup("count"));
    ASSERT_NE(nullptr, env.lookup("totals")->as_table().rows[0]->lookup("total"));
    EXPECT_EQ("150", env.lookup("totals")->as_table().rows[0]->lookup("total")->string());
}

TEST(RuntimeExecTest, ExecutesRenameDuplicateAndSetQueryFile) {
    auto file = ParseFile(R"(
        import "array"

        builtin duplicate : (<-tables: stream[A], column: string, as: string) => stream[A]
        builtin rename : (<-tables: stream[A], columns: A) => stream[B]
        builtin set : (<-tables: stream[A], key: string, value: string) => stream[A]

        shaped = array.from(
            bucket: "telegraf",
            rows: [
                {_measurement: "cpu", _value: 95.0, host: "a"},
                {_measurement: "mem", _value: 40.0, host: "b"},
            ],
        )
            |> duplicate(column: "_value", as: "raw_value")
            |> rename(columns: {_measurement: "measurement", _value: "usage"})
            |> set(key: "env", value: "prod")
        shaped
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("shaped").ok());
    ASSERT_EQ(Value::Type::Table, env.lookup("shaped")->type());
    ASSERT_EQ(2, env.lookup("shaped")->as_table().rows.size());
    const auto& row = env.lookup("shaped")->as_table().rows[0];
    ASSERT_NE(nullptr, row);
    EXPECT_EQ(nullptr, row->lookup("_measurement"));
    EXPECT_EQ(nullptr, row->lookup("_value"));
    EXPECT_EQ("\"cpu\"", row->lookup("measurement")->string());
    EXPECT_EQ("95", row->lookup("usage")->string());
    EXPECT_EQ("95", row->lookup("raw_value")->string());
    EXPECT_EQ("\"prod\"", row->lookup("env")->string());
    EXPECT_EQ(Value::Type::Table, result_or->last.value.type());
}

TEST(RuntimeExecTest, ExecutesSortGroupCountFirstAndLastQueryFile) {
    auto file = ParseFile(R"(
        import "array"

        builtin group : (<-tables: stream[A], ?columns: [string], ?mode: string) => stream[A]
        builtin sort : (<-tables: stream[A], columns: [string], desc: bool) => stream[A]
        builtin first : (<-tables: stream[A]) => stream[A]
        builtin last : (<-tables: stream[A]) => stream[A]
        builtin count : (<-tables: stream[A], column: string) => stream[A]

        hottest = array.from(
            bucket: "telegraf",
            rows: [
                {_measurement: "cpu", _value: 70.0, host: "b"},
                {_measurement: "cpu", _value: 95.0, host: "a"},
                {_measurement: "mem", _value: 40.0, host: "c"},
            ],
        )
            |> group(columns: ["_measurement"])
            |> sort(columns: ["_value"], desc: true)
            |> first()
        latest = array.from(bucket: "telegraf", rows: [{_value: 1}, {_value: 2}]) |> last()
        counted = array.from(bucket: "telegraf", rows: [
            {_measurement: "cpu", host: "a", _value: 1},
            {_measurement: "cpu", host: "a"},
            {_measurement: "mem", host: "b", _value: 2},
        ])
            |> group(columns: ["_measurement"])
            |> count(column: "_value")
        hottest
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_EQ(Value::Type::Table, result_or->last.value.type());
    ASSERT_TRUE(env.lookup("hottest").ok());
    ASSERT_EQ(2, env.lookup("hottest")->as_table().rows.size());
    ASSERT_EQ(2, env.lookup("hottest")->as_table().table_count());
    EXPECT_EQ("95", env.lookup("hottest")->as_table().rows[0]->lookup("_value")->string());
    ASSERT_NE(nullptr, env.lookup("hottest")->as_table().rows[0]->lookup("_group"));
    EXPECT_EQ("{_measurement: \"cpu\"}",
              env.lookup("hottest")->as_table().rows[0]->lookup("_group")->string());
    EXPECT_EQ("40", env.lookup("hottest")->as_table().rows[1]->lookup("_value")->string());
    ASSERT_TRUE(env.lookup("latest").ok());
    EXPECT_EQ("2", env.lookup("latest")->as_table().rows[0]->lookup("_value")->string());
    ASSERT_TRUE(env.lookup("counted").ok());
    ASSERT_EQ(2, env.lookup("counted")->as_table().rows.size());
    EXPECT_EQ("1", env.lookup("counted")->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("1", env.lookup("counted")->as_table().rows[1]->lookup("_value")->string());
}

TEST(RuntimeExecTest, ExecutesUnionAndJoinQueryFile) {
    auto file = ParseFile(R"(
        import "array"

        builtin union : (tables: [stream[A]]) => stream[A]
        builtin join : (tables: A, ?method: string, on: [string]) => stream[B]

        cpu = array.from(bucket: "cpu", rows: [
            {_time: "t1", _value: 90.0, host: "a"},
            {_time: "t2", _value: 70.0, host: "b"},
        ])
        mem = array.from(bucket: "mem", rows: [
            {_time: "t1", _value: 40.0},
            {_time: "t3", _value: 20.0},
        ])
        combined = union(tables: [cpu, mem])
        joined = join(tables: {cpu: cpu, mem: mem}, on: ["_time"])
        joined
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("combined").ok());
    EXPECT_EQ(4, env.lookup("combined")->as_table().rows.size());
    ASSERT_TRUE(env.lookup("joined").ok());
    ASSERT_EQ(1, env.lookup("joined")->as_table().rows.size());
    const auto& row = env.lookup("joined")->as_table().rows[0];
    ASSERT_NE(nullptr, row);
    EXPECT_EQ("\"t1\"", row->lookup("_time")->string());
    EXPECT_EQ("90", row->lookup("_value_cpu")->string());
    EXPECT_EQ("40", row->lookup("_value_mem")->string());
    EXPECT_EQ("\"a\"", row->lookup("host")->string());
    EXPECT_EQ(Value::Type::Table, result_or->last.value.type());
}

TEST(RuntimeExecTest, ExecutesJoinQueryFileUsingMatchingGroupInstancesOnly) {
    auto file = ParseFile(R"(
        import "array"

        builtin group : (<-tables: stream[A], ?columns: [string], ?mode: string) => stream[A]
        builtin join : (tables: A, ?method: string, on: [string]) => stream[B]

        cpu = array.from(bucket: "cpu", rows: [
            {_time: "t1", host: "a", region: "east", _value: 90.0},
            {_time: "t2", host: "a", region: "east", _value: 91.0},
            {_time: "t3", host: "b", region: "west", _value: 70.0},
            {host: "a", region: "east", _value: 999.0},
        ])
            |> group(columns: ["host"])
        mem = array.from(bucket: "mem", rows: [
            {_time: "t1", host: "a", region: "east", _value: 40.0},
            {_time: "t2", host: "a", region: "east", _value: 41.0},
            {_time: "t4", host: "c", region: "north", _value: 20.0},
            {host: "a", region: "east", _value: 111.0},
        ])
            |> group(columns: ["host"])
        joined = join(tables: {cpu: cpu, mem: mem}, method: "inner", on: ["_time"])
        joined
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("joined").ok());
    ASSERT_EQ(2, env.lookup("joined")->as_table().rows.size());
    ASSERT_EQ(1, env.lookup("joined")->as_table().table_count());
    const auto& row = env.lookup("joined")->as_table().rows[0];
    ASSERT_NE(nullptr, row);
    EXPECT_EQ("\"t1\"", row->lookup("_time")->string());
    EXPECT_EQ("90", row->lookup("_value_cpu")->string());
    EXPECT_EQ("40", row->lookup("_value_mem")->string());
    EXPECT_EQ("\"a\"", row->lookup("host_cpu")->string());
    EXPECT_EQ("\"a\"", row->lookup("host_mem")->string());
    EXPECT_EQ("\"east\"", row->lookup("region_cpu")->string());
    EXPECT_EQ("\"east\"", row->lookup("region_mem")->string());
    ASSERT_NE(nullptr, row->lookup("_group"));
    EXPECT_EQ("{host_cpu: \"a\", host_mem: \"a\"}", row->lookup("_group")->string());
    EXPECT_EQ(Value::Type::Table, result_or->last.value.type());
}

TEST(RuntimeExecTest, ExecutesWindowAndOuterJoinQueryFile) {
    auto file = ParseFile(R"(
        import "array"

        builtin range : (<-tables: stream[A], start: time, ?stop: time) => stream[A]
        builtin group : (<-tables: stream[A], ?columns: [string], ?mode: string) => stream[A]
        builtin window : (<-tables: stream[A], every: duration, ?createEmpty: bool) => stream[A]
        builtin join : (tables: A, ?method: string, on: [string]) => stream[B]

        cpu = array.from(bucket: "cpu", rows: [
            {_time: 2024-01-01T00:00:10Z, host: "a", _value: 90.0},
            {_time: 2024-01-01T00:02:05Z, host: "a", _value: 91.0},
        ])
            |> range(start: 2024-01-01T00:00:00Z, stop: 2024-01-01T00:03:00Z)
            |> group(columns: ["host"])
            |> window(every: 1m, createEmpty: true)

        mem = array.from(bucket: "mem", rows: [
            {_time: "t2", host: "a", _value: 40.0},
            {_time: "t3", host: "a", _value: 20.0},
        ])
        joined = join(
            tables: {
                cpu: array.from(bucket: "cpu", rows: [
                    {_time: "t1", host: "a", _value: 90.0},
                    {_time: "t2", host: "a", _value: 91.0},
                ]),
                mem: mem,
            },
            method: "full",
            on: ["_time"],
        )
        joined
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("cpu").ok());
    ASSERT_EQ(3, env.lookup("cpu")->as_table().table_count());
    ASSERT_TRUE(env.lookup("joined").ok());
    ASSERT_EQ(3, env.lookup("joined")->as_table().rows.size());
    EXPECT_EQ("\"t1\"", env.lookup("joined")->as_table().rows[0]->lookup("_time")->string());
    EXPECT_TRUE(env.lookup("joined")->as_table().rows[0]->lookup("_value_mem")->is_null());
    EXPECT_EQ("\"t3\"", env.lookup("joined")->as_table().rows[2]->lookup("_time")->string());
    EXPECT_TRUE(env.lookup("joined")->as_table().rows[2]->lookup("_value_cpu")->is_null());
    EXPECT_EQ(Value::Type::Table, result_or->last.value.type());
}

TEST(RuntimeExecTest, ExecutesRankingAndQuantileQueryFile) {
    auto file = ParseFile(R"(
        import "array"

        builtin group : (<-tables: stream[A], ?columns: [string], ?mode: string) => stream[A]
        builtin spread : (<-tables: stream[A], ?column: string) => stream[A]
        builtin quantile : (<-tables: stream[A], q: B, ?column: string) => stream[A]
        builtin median : (<-tables: stream[A], ?column: string) => stream[A]
        builtin top : (<-tables: stream[A], n: int, ?columns: [string]) => stream[A]
        builtin bottom : (<-tables: stream[A], n: int, ?columns: [string]) => stream[A]

        byHost = array.from(bucket: "cpu", rows: [
            {_time: "t1", host: "a", _value: 10.0},
            {_time: "t2", host: "a", _value: 25.0},
            {_time: "t3", host: "b", _value: 40.0},
            {_time: "t4", host: "b", _value: 50.0},
        ]) |> group(columns: ["host"])
        spreaded = byHost |> spread()
        q = array.from(bucket: "cpu", rows: [
            {_time: "t1", _value: 10.0},
            {_time: "t2", _value: 20.0},
            {_time: "t3", _value: 30.0},
            {_time: "t4", _value: 40.0},
        ]) |> quantile(q: 0.25)
        multiQ = array.from(bucket: "cpu", rows: [
            {_time: "t1", _value: 10.0},
            {_time: "t2", _value: 20.0},
            {_time: "t3", _value: 30.0},
            {_time: "t4", _value: 40.0},
        ]) |> quantile(q: [0.5, 0.75, 0.99, 0.999])
        med = array.from(bucket: "cpu", rows: [
            {_time: "t1", _value: 10.0},
            {_time: "t2", _value: 20.0},
            {_time: "t3", _value: 30.0},
            {_time: "t4", _value: 40.0},
        ]) |> median()
        highest = array.from(bucket: "cpu", rows: [
            {_time: "t1", _value: 10.0},
            {_time: "t2", _value: 20.0},
            {_time: "t3", _value: 30.0},
        ]) |> top(n: 2)
        lowest = array.from(bucket: "cpu", rows: [
            {_time: "t1", _value: 10.0},
            {_time: "t2", _value: 20.0},
            {_time: "t3", _value: 30.0},
        ]) |> bottom(n: 2)
        lowest
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("spreaded").ok());
    ASSERT_EQ(2, env.lookup("spreaded")->as_table().rows.size());
    EXPECT_EQ("15", env.lookup("spreaded")->as_table().rows[0]->lookup("_value")->string());
    ASSERT_TRUE(env.lookup("q").ok());
    EXPECT_EQ("17.5", env.lookup("q")->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("0.25", env.lookup("q")->as_table().rows[0]->lookup("quantile")->string());
    ASSERT_TRUE(env.lookup("multiQ").ok());
    ASSERT_EQ(4, env.lookup("multiQ")->as_table().rows.size());
    EXPECT_EQ("25", env.lookup("multiQ")->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("0.5", env.lookup("multiQ")->as_table().rows[0]->lookup("quantile")->string());
    EXPECT_EQ("39.97", env.lookup("multiQ")->as_table().rows[3]->lookup("_value")->string());
    EXPECT_EQ("0.999", env.lookup("multiQ")->as_table().rows[3]->lookup("quantile")->string());
    ASSERT_TRUE(env.lookup("med").ok());
    EXPECT_EQ("25", env.lookup("med")->as_table().rows[0]->lookup("_value")->string());
    ASSERT_TRUE(env.lookup("highest").ok());
    EXPECT_EQ("30", env.lookup("highest")->as_table().rows[0]->lookup("_value")->string());
    ASSERT_TRUE(env.lookup("lowest").ok());
    EXPECT_EQ("10", env.lookup("lowest")->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ(Value::Type::Table, result_or->last.value.type());
}

TEST(RuntimeExecTest, ExecutesDistinctQueryFile) {
    auto file = ParseFile(R"(
        import "array"

        builtin distinct : (<-tables: stream[A], ?column: string) => stream[A]

        hosts = array.from(bucket: "cpu", rows: [
            {_time: "t1", host: "a", region: "east", _value: 90.0},
            {_time: "t2", host: "a", region: "east", _value: 70.0},
            {_time: "t3", host: "b", region: "west", _value: 60.0},
        ])
            |> distinct(column: "host")
        hosts
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("hosts").ok());
    ASSERT_EQ(2, env.lookup("hosts")->as_table().rows.size());
    EXPECT_EQ("\"a\"", env.lookup("hosts")->as_table().rows[0]->lookup("host")->string());
    EXPECT_EQ("90", env.lookup("hosts")->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("\"b\"", env.lookup("hosts")->as_table().rows[1]->lookup("host")->string());
    EXPECT_EQ("60", env.lookup("hosts")->as_table().rows[1]->lookup("_value")->string());
    EXPECT_EQ(Value::Type::Table, result_or->last.value.type());
}

TEST(RuntimeExecTest, ExecutesTailQueryFile) {
    auto file = ParseFile(R"(
        import "array"

        builtin tail : (<-tables: stream[A], n: int, ?offset: int) => stream[A]

        recent = array.from(bucket: "cpu", rows: [
            {_time: "t1", host: "a", _value: 10.0},
            {_time: "t2", host: "b", _value: 20.0},
            {_time: "t3", host: "c", _value: 30.0},
            {_time: "t4", host: "d", _value: 40.0},
        ])
            |> tail(n: 2, offset: 1)
        recent
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("recent").ok());
    ASSERT_EQ(2, env.lookup("recent")->as_table().rows.size());
    EXPECT_EQ("\"b\"", env.lookup("recent")->as_table().rows[0]->lookup("host")->string());
    EXPECT_EQ("20", env.lookup("recent")->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("\"c\"", env.lookup("recent")->as_table().rows[1]->lookup("host")->string());
    EXPECT_EQ("30", env.lookup("recent")->as_table().rows[1]->lookup("_value")->string());
    EXPECT_EQ(Value::Type::Table, result_or->last.value.type());
}

TEST(RuntimeExecTest, ExecutesPivotQueryFile) {
    auto file = ParseFile(R"(
        import "array"

        builtin pivot : (<-tables: stream[A], rowKey: [string], columnKey: [string], valueColumn: string) => stream[B]

        wide = array.from(bucket: "cpu", rows: [
            {_time: 2024-01-01T00:01:00Z, host: "a", metric: "cpu", usage: 72.0},
            {_time: 2024-01-01T00:01:00Z, host: "a", metric: "mem", usage: 63.0},
            {_time: 2024-01-01T00:02:00Z, host: "a", metric: "cpu", usage: 82.0},
            {_time: 2024-01-01T00:02:00Z, host: "a", metric: "mem", usage: 68.0},
        ])
            |> pivot(rowKey: ["_time", "host"], columnKey: ["metric"], valueColumn: "usage")
        wide
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("wide").ok());
    ASSERT_EQ(2, env.lookup("wide")->as_table().rows.size());
    EXPECT_EQ("2024-01-01T00:01:00Z",
              env.lookup("wide")->as_table().rows[0]->lookup("_time")->string());
    EXPECT_EQ("72", env.lookup("wide")->as_table().rows[0]->lookup("cpu")->string());
    EXPECT_EQ("63", env.lookup("wide")->as_table().rows[0]->lookup("mem")->string());
    EXPECT_EQ("2024-01-01T00:02:00Z",
              env.lookup("wide")->as_table().rows[1]->lookup("_time")->string());
    EXPECT_EQ("82", env.lookup("wide")->as_table().rows[1]->lookup("cpu")->string());
    EXPECT_EQ("68", env.lookup("wide")->as_table().rows[1]->lookup("mem")->string());
    EXPECT_EQ(Value::Type::Table, result_or->last.value.type());
}

TEST(RuntimeExecTest, ExecutesFillQueryFile) {
    auto file = ParseFile(R"(
        import "array"

        builtin group : (<-tables: stream[A], columns: [string]) => stream[A]
        builtin fill : (<-tables: stream[A], ?column: string, ?usePrevious: bool, ?value: B) => stream[A]

        carried = array.from(bucket: "cpu", rows: [
            {_time: 2024-01-01T00:00:00Z, host: "a", _value: 10.0},
            {_time: 2024-01-01T00:01:00Z, host: "a"},
            {_time: 2024-01-01T00:02:00Z, host: "a", _value: 30.0},
            {_time: 2024-01-01T00:03:00Z, host: "b"},
        ])
            |> group(columns: ["host"])
            |> fill(usePrevious: true)
        defaults = array.from(bucket: "cpu", rows: [
            {_time: 2024-01-01T00:00:00Z, host: "a"},
            {_time: 2024-01-01T00:01:00Z, host: "a", usage: 2.0},
        ])
            |> fill(column: "usage", value: 0.0)
        carried
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("carried").ok());
    ASSERT_EQ(4, env.lookup("carried")->as_table().rows.size());
    EXPECT_EQ("10", env.lookup("carried")->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("10", env.lookup("carried")->as_table().rows[1]->lookup("_value")->string());
    EXPECT_EQ("30", env.lookup("carried")->as_table().rows[2]->lookup("_value")->string());
    EXPECT_EQ(nullptr, env.lookup("carried")->as_table().rows[3]->lookup("_value"));
    ASSERT_TRUE(env.lookup("defaults").ok());
    ASSERT_EQ(2, env.lookup("defaults")->as_table().rows.size());
    EXPECT_EQ("0", env.lookup("defaults")->as_table().rows[0]->lookup("usage")->string());
    EXPECT_EQ("2", env.lookup("defaults")->as_table().rows[1]->lookup("usage")->string());
    EXPECT_EQ(Value::Type::Table, result_or->last.value.type());
}

TEST(RuntimeExecTest, ExecutesElapsedQueryFile) {
    auto file = ParseFile(R"(
        import "array"

        builtin group : (<-tables: stream[A], columns: [string]) => stream[A]
        builtin elapsed : (<-tables: stream[A], ?unit: duration, ?timeColumn: string, ?columnName: string) => stream[A]

        deltas = array.from(bucket: "cpu", rows: [
            {_time: 2024-01-01T00:00:00Z, host: "a", _value: 10.0},
            {_time: 2024-01-01T00:00:10Z, host: "b", _value: 20.0},
            {_time: 2024-01-01T00:00:30Z, host: "a", _value: 30.0},
            {_time: 2024-01-01T00:01:00Z, host: "b", _value: 40.0},
        ])
            |> group(columns: ["host"])
            |> elapsed(unit: 10s)
        deltas
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("deltas").ok());
    ASSERT_EQ(2, env.lookup("deltas")->as_table().rows.size());
    EXPECT_EQ("\"a\"", env.lookup("deltas")->as_table().rows[0]->lookup("host")->string());
    EXPECT_EQ("3", env.lookup("deltas")->as_table().rows[0]->lookup("elapsed")->string());
    EXPECT_EQ("\"b\"", env.lookup("deltas")->as_table().rows[1]->lookup("host")->string());
    EXPECT_EQ("5", env.lookup("deltas")->as_table().rows[1]->lookup("elapsed")->string());
    EXPECT_EQ(Value::Type::Table, result_or->last.value.type());
}

TEST(RuntimeExecTest, ExecutesDifferenceQueryFile) {
    auto file = ParseFile(R"(
        import "array"

        builtin group : (<-tables: stream[A], columns: [string]) => stream[A]
        builtin difference : (<-tables: stream[A], ?column: string) => stream[A]

        deltas = array.from(bucket: "cpu", rows: [
            {_time: 2024-01-01T00:00:00Z, host: "a", _value: 10.0},
            {_time: 2024-01-01T00:00:10Z, host: "b", _value: 20.0},
            {_time: 2024-01-01T00:00:30Z, host: "a", _value: 17.0},
            {_time: 2024-01-01T00:01:00Z, host: "b", _value: 15.0},
        ])
            |> group(columns: ["host"])
            |> difference()
        deltas
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("deltas").ok());
    ASSERT_EQ(2, env.lookup("deltas")->as_table().rows.size());
    EXPECT_EQ("\"a\"", env.lookup("deltas")->as_table().rows[0]->lookup("host")->string());
    EXPECT_EQ("7", env.lookup("deltas")->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("\"b\"", env.lookup("deltas")->as_table().rows[1]->lookup("host")->string());
    EXPECT_EQ("-5", env.lookup("deltas")->as_table().rows[1]->lookup("_value")->string());
    EXPECT_EQ(Value::Type::Table, result_or->last.value.type());
}

TEST(RuntimeExecTest, ExecutesDifferenceQueryFileWithNonNegativeAndKeepFirst) {
    auto file = ParseFile(R"(
        import "array"

        builtin group : (<-tables: stream[A], columns: [string]) => stream[A]
        builtin difference : (<-tables: stream[A], ?column: string, ?nonNegative: bool, ?keepFirst: bool) => stream[A]

        deltas = array.from(bucket: "cpu", rows: [
            {_time: 2024-01-01T00:00:00Z, host: "a", _value: 10.0},
            {_time: 2024-01-01T00:00:20Z, host: "a", _value: 7.0},
            {_time: 2024-01-01T00:00:40Z, host: "a", _value: 13.0},
        ])
            |> group(columns: ["host"])
            |> difference(nonNegative: true, keepFirst: true)
        deltas
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("deltas").ok());
    ASSERT_EQ(3, env.lookup("deltas")->as_table().rows.size());
    EXPECT_TRUE(env.lookup("deltas")->as_table().rows[0]->lookup("_value")->is_null());
    EXPECT_TRUE(env.lookup("deltas")->as_table().rows[1]->lookup("_value")->is_null());
    EXPECT_EQ("6", env.lookup("deltas")->as_table().rows[2]->lookup("_value")->string());
    EXPECT_EQ(Value::Type::Table, result_or->last.value.type());
}

TEST(RuntimeExecTest, ExecutesDerivativeQueryFile) {
    auto file = ParseFile(R"(
        import "array"

        builtin group : (<-tables: stream[A], columns: [string]) => stream[A]
        builtin derivative : (<-tables: stream[A], ?unit: duration, ?column: string, ?timeColumn: string) => stream[A]

        rates = array.from(bucket: "cpu", rows: [
            {_time: 2024-01-01T00:00:00Z, host: "a", _value: 10.0},
            {_time: 2024-01-01T00:00:10Z, host: "b", _value: 20.0},
            {_time: 2024-01-01T00:00:30Z, host: "a", _value: 17.0},
            {_time: 2024-01-01T00:01:00Z, host: "b", _value: 15.0},
        ])
            |> group(columns: ["host"])
            |> derivative(unit: 10s)
        rates
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("rates").ok());
    ASSERT_EQ(2, env.lookup("rates")->as_table().rows.size());
    EXPECT_EQ("\"a\"", env.lookup("rates")->as_table().rows[0]->lookup("host")->string());
    EXPECT_EQ("2.33333333333333",
              env.lookup("rates")->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("\"b\"", env.lookup("rates")->as_table().rows[1]->lookup("host")->string());
    EXPECT_EQ("-1", env.lookup("rates")->as_table().rows[1]->lookup("_value")->string());
    EXPECT_EQ(Value::Type::Table, result_or->last.value.type());
}

TEST(RuntimeExecTest, ExecutesDerivativeQueryFileWithNonNegativeAndInitialZero) {
    auto file = ParseFile(R"(
        import "array"

        builtin group : (<-tables: stream[A], columns: [string]) => stream[A]
        builtin derivative : (<-tables: stream[A], ?unit: duration, ?column: string, ?timeColumn: string, ?nonNegative: bool, ?initialZero: bool) => stream[A]

        rates = array.from(bucket: "cpu", rows: [
            {_time: 2024-01-01T00:00:00Z, host: "a", _value: 10.0},
            {_time: 2024-01-01T00:00:10Z, host: "a", _value: 6.0},
            {_time: 2024-01-01T00:00:20Z, host: "a", _value: 8.0},
        ])
            |> group(columns: ["host"])
            |> derivative(unit: 10s, nonNegative: true, initialZero: true)
        rates
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("rates").ok());
    ASSERT_EQ(2, env.lookup("rates")->as_table().rows.size());
    EXPECT_EQ("6", env.lookup("rates")->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("2", env.lookup("rates")->as_table().rows[1]->lookup("_value")->string());
    EXPECT_EQ(Value::Type::Table, result_or->last.value.type());
}

TEST(RuntimeExecTest, ExecutesJoinStdlibPackageHelpers) {
    auto file = ParseFile(R"(
        import "array"

        import "join"
        builtin group : (<-tables: stream[A], ?columns: [string], ?mode: string) => stream[A]

        cpu = array.from(bucket: "cpu", rows: [
            {_time: "t1", host: "a", _value: 90.0},
            {_time: "t2", host: "a", _value: 91.0},
        ])
            |> group(columns: ["host"])
        mem = array.from(bucket: "mem", rows: [
            {_time: "t1", host: "a", _value: 40.0},
            {_time: "t3", host: "a", _value: 20.0},
        ])
            |> group(columns: ["host"])

        inner = join.inner(
            left: cpu,
            right: mem,
            on: (l, r) => l._time == r._time and l.host == r.host,
            as: (l, r) => ({_time: l._time, host: l.host, cpu: l._value, mem: r._value}),
        )
        left = join.left(
            left: cpu,
            right: mem,
            on: (l, r) => l._time == r._time and l.host == r.host,
            as: (l, r) => ({_time: l._time, host: l.host, cpu: l._value, mem: r._value}),
        )
        right = join.right(
            left: cpu,
            right: mem,
            on: (l, r) => l._time == r._time and l.host == r.host,
            as: (l, r) => ({_time: r._time, host: r.host, cpu: l._value, mem: r._value}),
        )
        full = join.full(
            left: cpu,
            right: mem,
            on: (l, r) => l._time == r._time and l.host == r.host,
            as: (l, r) => ({
                left_time: l._time,
                right_time: r._time,
                left_host: l.host,
                right_host: r.host,
                cpu: l._value,
                mem: r._value,
            }),
        )
        full
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("join").ok());
    EXPECT_EQ("{path: \"join\", inner: <builtin join.inner>, left: <builtin join.left>, "
              "right: <builtin join.right>, full: <builtin join.full>}",
              env.lookup("join")->string());
    ASSERT_TRUE(env.lookup("inner").ok());
    ASSERT_EQ(1, env.lookup("inner")->as_table().rows.size());
    EXPECT_EQ("90", env.lookup("inner")->as_table().rows[0]->lookup("cpu")->string());
    EXPECT_EQ("40", env.lookup("inner")->as_table().rows[0]->lookup("mem")->string());
    ASSERT_TRUE(env.lookup("left").ok());
    ASSERT_EQ(2, env.lookup("left")->as_table().rows.size());
    EXPECT_EQ("91", env.lookup("left")->as_table().rows[1]->lookup("cpu")->string());
    EXPECT_TRUE(env.lookup("left")->as_table().rows[1]->lookup("mem")->is_null());
    ASSERT_TRUE(env.lookup("right").ok());
    ASSERT_EQ(2, env.lookup("right")->as_table().rows.size());
    EXPECT_TRUE(env.lookup("right")->as_table().rows[1]->lookup("cpu")->is_null());
    EXPECT_EQ("20", env.lookup("right")->as_table().rows[1]->lookup("mem")->string());
    ASSERT_TRUE(env.lookup("full").ok());
    ASSERT_EQ(3, env.lookup("full")->as_table().rows.size());
    EXPECT_TRUE(env.lookup("full")->as_table().rows[1]->lookup("mem")->is_null());
    EXPECT_TRUE(env.lookup("full")->as_table().rows[2]->lookup("cpu")->is_null());
    EXPECT_EQ(Value::Type::Table, result_or->last.value.type());
}

TEST(RuntimeExecTest, ExecutesAggregateWindowQueryFile) {
    auto file = ParseFile(R"(
        import "array"

        builtin group : (<-tables: stream[A], columns: [string]) => stream[A]
        builtin mean : (values: [float]) => float
        builtin sum : (values: [float]) => float
        builtin count : (<-tables: stream[A], column: string) => stream[A]
        builtin aggregateWindow : (<-tables: stream[A], every: duration, fn: B) => stream[C]

        windowed = array.from(
            bucket: "telegraf",
            rows: [
                {_time: "2024-01-01T00:00:10Z", _value: 10.0, host: "a"},
                {_time: "2024-01-01T00:00:40Z", _value: 30.0, host: "a"},
                {_time: "2024-01-01T00:00:20Z", _value: 90.0, host: "b"},
                {_time: "2024-01-01T00:01:05Z", _value: 50.0, host: "a"},
            ],
        )
            |> group(columns: ["host"])
            |> aggregateWindow(every: 1m, fn: mean)
        usage = array.from(bucket: "telegraf", rows: [
                {_time: "2024-01-01T00:00:10Z", usage: 10.0},
                {_time: "2024-01-01T00:00:40Z", usage: 30.0},
                {_time: "2024-01-01T00:01:05Z", usage: 50.0},
            ])
            |> aggregateWindow(every: 1m, fn: sum, column: "usage")
        samples = array.from(bucket: "telegraf", rows: [
                {_time: "2024-01-01T00:00:10Z", _value: 10.0},
                {_time: "2024-01-01T00:00:40Z", _value: 30.0},
                {_time: "2024-01-01T00:01:05Z", _value: 50.0},
            ])
            |> aggregateWindow(every: 1m, fn: count)
        windowed
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("windowed").ok());
    ASSERT_EQ(Value::Type::Table, env.lookup("windowed")->type());
    ASSERT_EQ(3, env.lookup("windowed")->as_table().rows.size());
    const auto& first = env.lookup("windowed")->as_table().rows[0];
    ASSERT_NE(nullptr, first);
    EXPECT_EQ("20", first->lookup("_value")->string());
    EXPECT_EQ("{host: \"a\"}", first->lookup("_group")->string());
    EXPECT_EQ("2024-01-01T00:00:00Z", first->lookup("_start")->string());
    EXPECT_EQ("2024-01-01T00:01:00Z", first->lookup("_stop")->string());
    ASSERT_TRUE(env.lookup("usage").ok());
    ASSERT_EQ(2, env.lookup("usage")->as_table().rows.size());
    EXPECT_EQ("40", env.lookup("usage")->as_table().rows[0]->lookup("usage")->string());
    EXPECT_EQ("50", env.lookup("usage")->as_table().rows[1]->lookup("usage")->string());
    ASSERT_TRUE(env.lookup("samples").ok());
    ASSERT_EQ(2, env.lookup("samples")->as_table().rows.size());
    EXPECT_EQ("2", env.lookup("samples")->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("1", env.lookup("samples")->as_table().rows[1]->lookup("_value")->string());
    EXPECT_EQ(Value::Type::Table, result_or->last.value.type());
}

TEST(RuntimeExecTest, ExecutesAggregateWindowCreateEmptyQueryFile) {
    auto file = ParseFile(R"(
        import "array"

        builtin group : (<-tables: stream[A], columns: [string]) => stream[A]
        builtin mean : (values: [float]) => float
        builtin count : (<-tables: stream[A], column: string) => stream[A]
        builtin aggregateWindow : (<-tables: stream[A], every: duration, fn: B) => stream[C]

        padded = array.from(
            bucket: "telegraf",
            rows: [
                {_time: "2024-01-01T00:00:10Z", _value: 10.0, host: "a"},
                {_time: "2024-01-01T00:02:05Z", _value: 30.0, host: "a"},
            ],
        )
            |> group(columns: ["host"])
            |> aggregateWindow(every: 1m, fn: mean, createEmpty: true)
        counts = array.from(
            bucket: "telegraf",
            rows: [
                {_time: "2024-01-01T00:00:10Z", _value: 10.0},
                {_time: "2024-01-01T00:02:05Z", _value: 30.0},
            ],
        )
            |> aggregateWindow(every: 1m, fn: count, createEmpty: true)
        padded
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("padded").ok());
    ASSERT_EQ(3, env.lookup("padded")->as_table().rows.size());
    EXPECT_EQ("10", env.lookup("padded")->as_table().rows[0]->lookup("_value")->string());
    EXPECT_TRUE(env.lookup("padded")->as_table().rows[1]->lookup("_value")->is_null());
    EXPECT_EQ("2024-01-01T00:02:00Z",
              env.lookup("padded")->as_table().rows[1]->lookup("_time")->string());
    EXPECT_EQ("30", env.lookup("padded")->as_table().rows[2]->lookup("_value")->string());
    ASSERT_TRUE(env.lookup("counts").ok());
    ASSERT_EQ(3, env.lookup("counts")->as_table().rows.size());
    EXPECT_EQ("1", env.lookup("counts")->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("0", env.lookup("counts")->as_table().rows[1]->lookup("_value")->string());
    EXPECT_EQ("1", env.lookup("counts")->as_table().rows[2]->lookup("_value")->string());
}

TEST(RuntimeExecTest, ExecutesAggregateWindowOffsetQueryFile) {
    auto file = ParseFile(R"(
        import "array"

        builtin mean : (values: [float]) => float
        builtin aggregateWindow : (<-tables: stream[A], every: duration, fn: B) => stream[C]

        shifted = array.from(
            bucket: "telegraf",
            rows: [
                {_time: "2024-01-01T00:00:10Z", _value: 10.0},
                {_time: "2024-01-01T00:00:40Z", _value: 30.0},
                {_time: "2024-01-01T00:01:10Z", _value: 50.0},
            ],
        )
            |> aggregateWindow(every: 1m, offset: 30s, fn: mean)
        shifted
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("shifted").ok());
    ASSERT_EQ(2, env.lookup("shifted")->as_table().rows.size());
    EXPECT_EQ("10", env.lookup("shifted")->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("2023-12-31T23:59:30Z",
              env.lookup("shifted")->as_table().rows[0]->lookup("_start")->string());
    EXPECT_EQ("2024-01-01T00:00:30Z",
              env.lookup("shifted")->as_table().rows[0]->lookup("_stop")->string());
    EXPECT_EQ("40", env.lookup("shifted")->as_table().rows[1]->lookup("_value")->string());
}

TEST(RuntimeExecTest, ExecutesAggregateWindowCalendarMonthQueryFile) {
    auto file = ParseFile(R"(
        import "array"

        builtin mean : (values: [float]) => float
        builtin aggregateWindow : (<-tables: stream[A], every: duration, fn: B) => stream[C]

        monthly = array.from(
            bucket: "telegraf",
            rows: [
                {_time: "2024-01-15T08:00:00Z", _value: 10.0},
                {_time: "2024-01-20T09:30:00Z", _value: 30.0},
                {_time: "2024-02-02T00:15:00Z", _value: 50.0},
            ],
        )
            |> aggregateWindow(every: 1mo, fn: mean)
        monthly
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("monthly").ok());
    ASSERT_EQ(2, env.lookup("monthly")->as_table().rows.size());
    EXPECT_EQ("20", env.lookup("monthly")->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("2024-01-01T00:00:00Z",
              env.lookup("monthly")->as_table().rows[0]->lookup("_start")->string());
    EXPECT_EQ("2024-02-01T00:00:00Z",
              env.lookup("monthly")->as_table().rows[0]->lookup("_stop")->string());
    EXPECT_EQ("50", env.lookup("monthly")->as_table().rows[1]->lookup("_value")->string());
    EXPECT_EQ("2024-02-01T00:00:00Z",
              env.lookup("monthly")->as_table().rows[1]->lookup("_start")->string());
    EXPECT_EQ("2024-03-01T00:00:00Z",
              env.lookup("monthly")->as_table().rows[1]->lookup("_stop")->string());
}

TEST(RuntimeExecTest, ExecutesAggregateWindowTimezoneQueryFile) {
    auto file = ParseFile(R"(
        import "array"

        builtin group : (<-tables: stream[A], columns: [string]) => stream[A]
        builtin mean : (values: [float]) => float
        builtin aggregateWindow : (<-tables: stream[A], every: duration, fn: B) => stream[C]

        monthly = array.from(
            bucket: "telegraf",
            rows: [
                {
                    _time: "2024-03-15T12:00:00Z",
                    _value: 10.0,
                    host: "edge-1",
                    region: "us-west",
                    note: "drop-me",
                },
            ],
        )
            |> group(columns: ["host"])
            |> aggregateWindow(
                every: 1mo,
                fn: mean,
                location: {zone: "America/Los_Angeles", offset: 0s},
                timeSrc: "_start",
                timeDst: "bucket_time",
            )
        monthly
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_TRUE(env.lookup("monthly").ok());
    ASSERT_EQ(1, env.lookup("monthly")->as_table().rows.size());
    const auto& row = env.lookup("monthly")->as_table().rows[0];
    ASSERT_NE(nullptr, row);
    EXPECT_EQ("2024-03-01T08:00:00Z", row->lookup("_start")->string());
    EXPECT_EQ("2024-04-01T07:00:00Z", row->lookup("_stop")->string());
    EXPECT_EQ("2024-03-01T08:00:00Z", row->lookup("bucket_time")->string());
    EXPECT_EQ(nullptr, row->lookup("_time"));
    EXPECT_EQ(nullptr, row->lookup("region"));
    EXPECT_EQ(nullptr, row->lookup("note"));
}

TEST(RuntimeExecTest, UsesYieldNameForResultCollection) {
    auto file = ParseFile(R"(
        import "array"

        builtin yield : (<-tables: stream[A], ?name: string) => stream[A]

        array.from(
            bucket: "telegraf",
            rows: [{_time: "2024-01-01T00:00:00Z", _value: 95.0}],
        )
            |> yield(name: "cpu")
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_EQ(2, result_or->results.size());
    EXPECT_EQ("builtin.yield", result_or->results[0].name);
    EXPECT_EQ("cpu", result_or->results[1].name);
    ASSERT_EQ(Value::Type::Table, result_or->results[1].value.type());
    ASSERT_TRUE(result_or->results[1].value.as_table().result_name.has_value());
    EXPECT_EQ("cpu", *result_or->results[1].value.as_table().result_name);
}

} // namespace
} // namespace pl::flux
