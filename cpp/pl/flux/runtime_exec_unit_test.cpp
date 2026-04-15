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

} // namespace
} // namespace pl
