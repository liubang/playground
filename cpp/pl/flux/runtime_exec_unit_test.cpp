// Copyright (c) 2023 The Authors. All rights reserved.
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

#include "cpp/pl/flux/parser.h"
#include "cpp/pl/flux/runtime_exec.h"
#include <gtest/gtest.h>

namespace pl {
namespace {

std::unique_ptr<File> ParseFile(const std::string& source) {
    Parser parser(source);
    auto file = parser.parse_file("exec_test.flux");
    EXPECT_TRUE(parser.errors().empty()) << ::testing::PrintToString(parser.errors());
    return file;
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
    EXPECT_EQ(
        "{name: \"math\", success: true, extends: \"base\", value: 6}",
        testcase_or->value.string());
    EXPECT_FALSE(env.lookup("temp").ok());

    auto result_or = env.lookup_option("__flux.testcase.math");
    ASSERT_TRUE(result_or.ok()) << result_or.status();
    EXPECT_EQ(
        "{name: \"math\", success: true, extends: \"base\", value: 6}",
        result_or->string());
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
    EXPECT_EQ("{path: \"array\"}", env.lookup("array")->string());
    ASSERT_TRUE(env.lookup("regexp").ok());
    EXPECT_EQ("{path: \"regexp\", alias: \"regexp\"}", env.lookup("regexp")->string());
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
    EXPECT_EQ("{name: \"checks\", success: true, value: 15}",
              result_or->results[1].value.string());
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
        builtin from : (bucket: string) => stream[A]
        builtin range : (<-tables: stream[A], start: time, ?stop: time) => stream[A]
        builtin filter : (<-tables: stream[A], fn: (r: A) => bool) => stream[A]
        builtin map : (<-tables: stream[A], fn: (r: A) => B) => stream[B]

        result = from(
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
        builtin from : (bucket: string) => stream[A]
        builtin limit : (<-tables: stream[A], n: int) => stream[A]
        builtin keep : (<-tables: stream[A], columns: [string]) => stream[A]
        builtin reduce : (<-tables: stream[A], identity: B, fn: (r: A, accumulator: B) => B) => stream[B]
        builtin drop : (<-tables: stream[A], columns: [string]) => stream[A]

        totals = from(
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
        builtin from : (bucket: string) => stream[A]
        builtin duplicate : (<-tables: stream[A], column: string, as: string) => stream[A]
        builtin rename : (<-tables: stream[A], columns: A) => stream[B]
        builtin set : (<-tables: stream[A], key: string, value: string) => stream[A]

        shaped = from(
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
        builtin from : (bucket: string) => stream[A]
        builtin group : (<-tables: stream[A], columns: [string]) => stream[A]
        builtin sort : (<-tables: stream[A], columns: [string], desc: bool) => stream[A]
        builtin first : (<-tables: stream[A]) => stream[A]
        builtin last : (<-tables: stream[A]) => stream[A]
        builtin count : (<-tables: stream[A], column: string) => stream[A]

        hottest = from(
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
        latest = from(bucket: "telegraf", rows: [{_value: 1}, {_value: 2}]) |> last()
        counted = from(bucket: "telegraf", rows: [{_value: 1}, {_value: 2}, {host: "missing"}])
            |> count(column: "_value")
        hottest
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_EQ(Value::Type::Table, result_or->last.value.type());
    ASSERT_TRUE(env.lookup("hottest").ok());
    ASSERT_EQ(1, env.lookup("hottest")->as_table().rows.size());
    EXPECT_EQ("95", env.lookup("hottest")->as_table().rows[0]->lookup("_value")->string());
    ASSERT_NE(nullptr, env.lookup("hottest")->as_table().rows[0]->lookup("_group"));
    EXPECT_EQ("{_measurement: \"cpu\"}",
              env.lookup("hottest")->as_table().rows[0]->lookup("_group")->string());
    ASSERT_TRUE(env.lookup("latest").ok());
    EXPECT_EQ("2", env.lookup("latest")->as_table().rows[0]->lookup("_value")->string());
    ASSERT_TRUE(env.lookup("counted").ok());
    EXPECT_EQ("2", env.lookup("counted")->as_table().rows[0]->lookup("_value")->string());
}

TEST(RuntimeExecTest, ExecutesUnionAndJoinQueryFile) {
    auto file = ParseFile(R"(
        builtin from : (bucket: string) => stream[A]
        builtin union : (tables: [stream[A]]) => stream[A]
        builtin join : (tables: A, on: [string]) => stream[B]

        cpu = from(bucket: "cpu", rows: [
            {_time: "t1", _value: 90.0, host: "a"},
            {_time: "t2", _value: 70.0, host: "b"},
        ])
        mem = from(bucket: "mem", rows: [
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
    EXPECT_EQ("90", row->lookup("cpu._value")->string());
    EXPECT_EQ("40", row->lookup("mem._value")->string());
    EXPECT_EQ("\"a\"", row->lookup("cpu.host")->string());
    EXPECT_EQ(Value::Type::Table, result_or->last.value.type());
}

TEST(RuntimeExecTest, ExecutesDistinctQueryFile) {
    auto file = ParseFile(R"(
        builtin from : (bucket: string) => stream[A]
        builtin distinct : (<-tables: stream[A], ?column: string) => stream[A]

        hosts = from(bucket: "cpu", rows: [
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
        builtin from : (bucket: string) => stream[A]
        builtin tail : (<-tables: stream[A], n: int, ?offset: int) => stream[A]

        recent = from(bucket: "cpu", rows: [
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
        builtin from : (bucket: string) => stream[A]
        builtin pivot : (<-tables: stream[A], rowKey: [string], columnKey: [string], valueColumn: string) => stream[B]

        wide = from(bucket: "cpu", rows: [
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
    EXPECT_EQ("2024-01-01T00:01:00Z", env.lookup("wide")->as_table().rows[0]->lookup("_time")->string());
    EXPECT_EQ("72", env.lookup("wide")->as_table().rows[0]->lookup("cpu")->string());
    EXPECT_EQ("63", env.lookup("wide")->as_table().rows[0]->lookup("mem")->string());
    EXPECT_EQ("2024-01-01T00:02:00Z", env.lookup("wide")->as_table().rows[1]->lookup("_time")->string());
    EXPECT_EQ("82", env.lookup("wide")->as_table().rows[1]->lookup("cpu")->string());
    EXPECT_EQ("68", env.lookup("wide")->as_table().rows[1]->lookup("mem")->string());
    EXPECT_EQ(Value::Type::Table, result_or->last.value.type());
}

TEST(RuntimeExecTest, ExecutesFillQueryFile) {
    auto file = ParseFile(R"(
        builtin from : (bucket: string) => stream[A]
        builtin group : (<-tables: stream[A], columns: [string]) => stream[A]
        builtin fill : (<-tables: stream[A], ?column: string, ?usePrevious: bool, ?value: B) => stream[A]

        carried = from(bucket: "cpu", rows: [
            {_time: 2024-01-01T00:00:00Z, host: "a", _value: 10.0},
            {_time: 2024-01-01T00:01:00Z, host: "a"},
            {_time: 2024-01-01T00:02:00Z, host: "a", _value: 30.0},
            {_time: 2024-01-01T00:03:00Z, host: "b"},
        ])
            |> group(columns: ["host"])
            |> fill(usePrevious: true)
        defaults = from(bucket: "cpu", rows: [
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
        builtin from : (bucket: string) => stream[A]
        builtin group : (<-tables: stream[A], columns: [string]) => stream[A]
        builtin elapsed : (<-tables: stream[A], ?unit: duration, ?timeColumn: string, ?columnName: string) => stream[A]

        deltas = from(bucket: "cpu", rows: [
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
        builtin from : (bucket: string) => stream[A]
        builtin group : (<-tables: stream[A], columns: [string]) => stream[A]
        builtin difference : (<-tables: stream[A], ?column: string) => stream[A]

        deltas = from(bucket: "cpu", rows: [
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
        builtin from : (bucket: string) => stream[A]
        builtin group : (<-tables: stream[A], columns: [string]) => stream[A]
        builtin difference : (<-tables: stream[A], ?column: string, ?nonNegative: bool, ?keepFirst: bool) => stream[A]

        deltas = from(bucket: "cpu", rows: [
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
        builtin from : (bucket: string) => stream[A]
        builtin group : (<-tables: stream[A], columns: [string]) => stream[A]
        builtin derivative : (<-tables: stream[A], ?unit: duration, ?column: string, ?timeColumn: string) => stream[A]

        rates = from(bucket: "cpu", rows: [
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
    EXPECT_EQ("2.33333333333333", env.lookup("rates")->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("\"b\"", env.lookup("rates")->as_table().rows[1]->lookup("host")->string());
    EXPECT_EQ("-1", env.lookup("rates")->as_table().rows[1]->lookup("_value")->string());
    EXPECT_EQ(Value::Type::Table, result_or->last.value.type());
}

TEST(RuntimeExecTest, ExecutesDerivativeQueryFileWithNonNegativeAndInitialZero) {
    auto file = ParseFile(R"(
        builtin from : (bucket: string) => stream[A]
        builtin group : (<-tables: stream[A], columns: [string]) => stream[A]
        builtin derivative : (<-tables: stream[A], ?unit: duration, ?column: string, ?timeColumn: string, ?nonNegative: bool, ?initialZero: bool) => stream[A]

        rates = from(bucket: "cpu", rows: [
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

TEST(RuntimeExecTest, ExecutesAggregateWindowQueryFile) {
    auto file = ParseFile(R"(
        builtin from : (bucket: string) => stream[A]
        builtin group : (<-tables: stream[A], columns: [string]) => stream[A]
        builtin mean : (values: [float]) => float
        builtin sum : (values: [float]) => float
        builtin count : (<-tables: stream[A], column: string) => stream[A]
        builtin aggregateWindow : (<-tables: stream[A], every: duration, fn: B) => stream[C]

        windowed = from(
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
        usage = from(bucket: "telegraf", rows: [
                {_time: "2024-01-01T00:00:10Z", usage: 10.0},
                {_time: "2024-01-01T00:00:40Z", usage: 30.0},
                {_time: "2024-01-01T00:01:05Z", usage: 50.0},
            ])
            |> aggregateWindow(every: 1m, fn: sum, column: "usage")
        samples = from(bucket: "telegraf", rows: [
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
        builtin from : (bucket: string) => stream[A]
        builtin group : (<-tables: stream[A], columns: [string]) => stream[A]
        builtin mean : (values: [float]) => float
        builtin count : (<-tables: stream[A], column: string) => stream[A]
        builtin aggregateWindow : (<-tables: stream[A], every: duration, fn: B) => stream[C]

        padded = from(
            bucket: "telegraf",
            rows: [
                {_time: "2024-01-01T00:00:10Z", _value: 10.0, host: "a"},
                {_time: "2024-01-01T00:02:05Z", _value: 30.0, host: "a"},
            ],
        )
            |> group(columns: ["host"])
            |> aggregateWindow(every: 1m, fn: mean, createEmpty: true)
        counts = from(
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
        builtin from : (bucket: string) => stream[A]
        builtin mean : (values: [float]) => float
        builtin aggregateWindow : (<-tables: stream[A], every: duration, fn: B) => stream[C]

        shifted = from(
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
        builtin from : (bucket: string) => stream[A]
        builtin mean : (values: [float]) => float
        builtin aggregateWindow : (<-tables: stream[A], every: duration, fn: B) => stream[C]

        monthly = from(
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
        builtin from : (bucket: string) => stream[A]
        builtin group : (<-tables: stream[A], columns: [string]) => stream[A]
        builtin mean : (values: [float]) => float
        builtin aggregateWindow : (<-tables: stream[A], every: duration, fn: B) => stream[C]

        monthly = from(
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
        builtin from : (bucket: string) => stream[A]
        builtin yield : (<-tables: stream[A], ?name: string) => stream[A]

        from(
            bucket: "telegraf",
            rows: [{_time: "2024-01-01T00:00:00Z", _value: 95.0}],
        )
            |> yield(name: "cpu")
    )");
    ASSERT_NE(file, nullptr);

    Environment env;
    auto result_or = StatementExecutor::ExecuteFile(*file, env);

    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_EQ(3, result_or->results.size());
    EXPECT_EQ("builtin.from", result_or->results[0].name);
    EXPECT_EQ("builtin.yield", result_or->results[1].name);
    EXPECT_EQ("cpu", result_or->results[2].name);
    ASSERT_EQ(Value::Type::Table, result_or->results[2].value.type());
    ASSERT_TRUE(result_or->results[2].value.as_table().result_name.has_value());
    EXPECT_EQ("cpu", *result_or->results[2].value.as_table().result_name);
}

} // namespace
} // namespace pl
