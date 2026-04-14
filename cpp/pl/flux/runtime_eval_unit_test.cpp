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
#include "cpp/pl/flux/runtime_builtin.h"
#include "cpp/pl/flux/runtime_eval.h"
#include <gtest/gtest.h>

namespace pl {
namespace {

const Expression& ParseAssignmentInit(const std::string& source) {
    static std::unique_ptr<File> file_holder;
    Parser parser(source);
    file_holder = parser.parse_file("eval_test.flux");
    EXPECT_TRUE(parser.errors().empty()) << ::testing::PrintToString(parser.errors());
    EXPECT_EQ(1, file_holder->body.size());
    const auto& assignment = std::get<std::unique_ptr<VariableAssgn>>(file_holder->body[0]->stmt);
    EXPECT_NE(assignment, nullptr);
    return *assignment->init;
}

TEST(RuntimeEvalTest, EvaluatesLiteralsArithmeticAndConditionalExpressions) {
    Environment env;
    const auto& expr =
        ParseAssignmentInit("result = if 1 + 2 * 3 > 5 and not false then \"hot\" else \"ok\"");

    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(Value::Type::String, result->type());
    EXPECT_EQ("\"hot\"", result->string());
}

TEST(RuntimeEvalTest, EvaluatesIdentifiersMembersIndexesAndExists) {
    Environment env;
    env.define("config", Value::object({
                             {"enabled", Value::boolean(true)},
                             {"tags", Value::array({Value::string("cpu"), Value::string("mem")})},
                         }));
    const auto& expr = ParseAssignmentInit(
        "result = if exists config.enabled then config.tags[1] else \"missing\"");

    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ("\"mem\"", result->string());
}

TEST(RuntimeEvalTest, EvaluatesStringInterpolationAndRegexMatches) {
    Environment env;
    env.define("host", Value::string("local"));
    const auto& expr = ParseAssignmentInit(
        "result = if \"cpu-total\" =~ /cpu.*/ then \"host ${host}\" else \"miss\"");

    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ("\"host local\"", result->string());
}

TEST(RuntimeEvalTest, EvaluatesRecordUpdateWithObjectSource) {
    Environment env;
    env.define("base", Value::object({
                           {"enabled", Value::boolean(false)},
                           {"host", Value::string("local")},
                       }));
    const auto& expr = ParseAssignmentInit("result = {base with enabled: true, level: \"hot\"}");

    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ("{enabled: true, host: \"local\", level: \"hot\"}", result->string());
}

TEST(RuntimeEvalTest, ReportsMissingCallBindings) {
    Environment env;
    const auto& expr = ParseAssignmentInit("result = range(start: -1h)");

    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(absl::StatusCode::kNotFound, result.status().code());
}

TEST(RuntimeEvalTest, EvaluatesBuiltinCalls) {
    Environment env;
    BuiltinRegistry::Install(env);
    const auto& expr = ParseAssignmentInit("result = len([1, 2, 3])");

    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(Value::integer(3), *result);
}

TEST(RuntimeEvalTest, EvaluatesBuiltinCallsWithNamedArgumentObject) {
    Environment env;
    BuiltinRegistry::Install(env);
    const auto& expr =
        ParseAssignmentInit("result = contains(set: [\"cpu\", \"mem\"], value: \"cpu\")");

    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(Value::boolean(true), *result);
}

TEST(RuntimeEvalTest, EvaluatesAggregateBuiltins) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& sum_expr = ParseAssignmentInit("result = sum([1, 2, 3])");
    auto sum = ExpressionEvaluator::Evaluate(sum_expr, env);
    ASSERT_TRUE(sum.ok()) << sum.status();
    EXPECT_EQ(Value::integer(6), *sum);

    const auto& mean_expr = ParseAssignmentInit("result = mean([1, 2, 3, 4])");
    auto mean = ExpressionEvaluator::Evaluate(mean_expr, env);
    ASSERT_TRUE(mean.ok()) << mean.status();
    EXPECT_EQ(Value::floating(2.5), *mean);

    const auto& min_expr = ParseAssignmentInit("result = min([3, 1, 4])");
    auto min = ExpressionEvaluator::Evaluate(min_expr, env);
    ASSERT_TRUE(min.ok()) << min.status();
    EXPECT_EQ(Value::integer(1), *min);

    const auto& max_expr = ParseAssignmentInit("result = max([3.0, 1.5, 4.5])");
    auto max = ExpressionEvaluator::Evaluate(max_expr, env);
    ASSERT_TRUE(max.ok()) << max.status();
    EXPECT_EQ(Value::floating(4.5), *max);
}

TEST(RuntimeEvalTest, EvaluatesUserFunctionCallsWithClosureCapture) {
    Environment env;
    env.define("base", Value::integer(2));
    const auto& expr = ParseAssignmentInit("result = ((x) => x + base)(3)");

    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(Value::integer(5), *result);
}

TEST(RuntimeEvalTest, EvaluatesUserFunctionCallsWithDefaultArgument) {
    Environment env;
    const auto& expr = ParseAssignmentInit("result = ((?limit=5) => limit + 1)()");

    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(Value::integer(6), *result);
}

TEST(RuntimeEvalTest, EvaluatesUserFunctionCallsWithNamedArguments) {
    Environment env;
    const auto& expr =
        ParseAssignmentInit("result = ((value, ?inc=1) => value + inc)(value: 4, inc: 2)");

    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(Value::integer(6), *result);
}

TEST(RuntimeEvalTest, EvaluatesBlockBodiedUserFunctions) {
    Environment env;
    const auto& expr = ParseAssignmentInit("result = ((x) => { return x + 1 })(2)");

    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(Value::integer(3), *result);
}

TEST(RuntimeEvalTest, EvaluatesPipeIntoBuiltinWithoutExplicitArguments) {
    Environment env;
    BuiltinRegistry::Install(env);
    const auto& expr = ParseAssignmentInit("result = [1, 2, 3] |> len()");

    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(Value::integer(3), *result);
}

TEST(RuntimeEvalTest, EvaluatesPipeIntoBuiltinNamedPipeParameter) {
    Environment env;
    BuiltinRegistry::Install(env);
    const auto& expr =
        ParseAssignmentInit("result = [\"cpu\", \"mem\"] |> contains(value: \"cpu\")");

    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(Value::boolean(true), *result);
}

TEST(RuntimeEvalTest, EvaluatesPipeIntoAggregateBuiltin) {
    Environment env;
    BuiltinRegistry::Install(env);
    const auto& expr = ParseAssignmentInit("result = [1, 2, 3, 4] |> mean()");

    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(Value::floating(2.5), *result);
}

TEST(RuntimeEvalTest, EvaluatesInMemoryQueryPipelineBuiltins) {
    Environment env;
    BuiltinRegistry::Install(env);
    const auto& expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_time: "2024-01-01T00:00:00Z", _measurement: "cpu", _value: 92.0, host: "a"},
                {_time: "2024-01-02T00:00:00Z", _measurement: "mem", _value: 50.0, host: "a"},
                {_time: "2024-01-03T00:00:00Z", _measurement: "cpu", _value: 70.0, host: "b"},
            ],
        )
            |> range(start: 2024-01-01T00:00:00Z, stop: 2024-01-02T12:00:00Z)
            |> filter(fn: (r) => r._measurement == "cpu" and r._value > 80.0)
            |> map(fn: (r) => ({r with level: if r._value > 90.0 then "hot" else "ok"}))
    )");

    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    EXPECT_EQ("telegraf", result->as_table().bucket);
    ASSERT_EQ(1, result->as_table().rows.size());
    ASSERT_NE(nullptr, result->as_table().rows[0]);
    EXPECT_EQ("\"cpu\"", result->as_table().rows[0]->lookup("_measurement")->string());
    EXPECT_EQ("\"hot\"", result->as_table().rows[0]->lookup("level")->string());
}

TEST(RuntimeEvalTest, EvaluatesPipeIntoUserFunctionPipeParameter) {
    Environment env;
    const auto& expr = ParseAssignmentInit("result = 3 |> ((<-value, ?inc=1) => value + inc)(inc: 2)");

    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(Value::integer(5), *result);
}

TEST(RuntimeEvalTest, ReportsMissingBindings) {
    Environment env;
    const auto& expr = ParseAssignmentInit("result = missing + 1");

    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(absl::StatusCode::kNotFound, result.status().code());
}

} // namespace
} // namespace pl
