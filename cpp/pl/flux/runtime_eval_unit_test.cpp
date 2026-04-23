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

#include "cpp/pl/flux/parser.h"
#include "cpp/pl/flux/runtime_builtin.h"
#include "cpp/pl/flux/runtime_eval.h"
#include <cstdio>
#include <fstream>
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

TEST(RuntimeEvalTest, EvaluatesUnaryMinusForDurationLiterals) {
    Environment env;
    const auto& expr = ParseAssignmentInit("result = -1h");

    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(Value::Type::Duration, result->type());
    EXPECT_EQ("-1h", result->string());
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

TEST(RuntimeEvalTest, EvaluatesTableHelperBuiltins) {
    Environment env;
    BuiltinRegistry::Install(env);
    const auto& expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_time: "2024-01-01T00:00:00Z", host: "edge-1", region: "us-east", _value: 70},
                {_time: "2024-01-01T00:01:00Z", host: "edge-2", region: "us-west", _value: 91},
            ],
        )
            |> group(columns: ["host", "region"])
            |> findColumn(fn: (r) => r.region == "us-west", column: "_value")
    )");

    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Array, result->type());
    ASSERT_EQ(1, result->as_array().elements.size());
    EXPECT_EQ("91", result->as_array().elements[0].string());
}

TEST(RuntimeEvalTest, EvaluatesColumnsKeysAndFindRecordBuiltins) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& columns_expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_time: "2024-01-01T00:00:00Z", host: "edge-1", region: "us-east", _value: 70},
            ],
        )
            |> group(columns: ["host", "region"])
            |> columns()
    )");
    auto columns = ExpressionEvaluator::Evaluate(columns_expr, env);
    ASSERT_TRUE(columns.ok()) << columns.status();
    ASSERT_EQ(Value::Type::Table, columns->type());
    ASSERT_EQ(4, columns->as_table().rows.size());
    EXPECT_EQ("\"_time\"", columns->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("\"host\"", columns->as_table().rows[1]->lookup("_value")->string());
    EXPECT_EQ("\"region\"", columns->as_table().rows[2]->lookup("_value")->string());
    EXPECT_EQ("\"_value\"", columns->as_table().rows[3]->lookup("_value")->string());

    const auto& keys_expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_time: "2024-01-01T00:00:00Z", host: "edge-1", region: "us-east", _value: 70},
            ],
        )
            |> group(columns: ["host", "region"])
            |> keys()
    )");
    auto keys = ExpressionEvaluator::Evaluate(keys_expr, env);
    ASSERT_TRUE(keys.ok()) << keys.status();
    ASSERT_EQ(Value::Type::Table, keys->type());
    ASSERT_EQ(2, keys->as_table().rows.size());
    EXPECT_EQ("\"host\"", keys->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("\"region\"", keys->as_table().rows[1]->lookup("_value")->string());

    const auto& record_expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_time: "2024-01-01T00:00:00Z", host: "edge-1", region: "us-east", _value: 70},
                {_time: "2024-01-01T00:01:00Z", host: "edge-2", region: "us-west", _value: 91},
            ],
        )
            |> findRecord(fn: (r) => true, idx: 1)
    )");
    auto record = ExpressionEvaluator::Evaluate(record_expr, env);
    ASSERT_TRUE(record.ok()) << record.status();
    ASSERT_EQ(Value::Type::Object, record->type());
    ASSERT_NE(nullptr, record->as_object().lookup("host"));
    ASSERT_NE(nullptr, record->as_object().lookup("_value"));
    EXPECT_EQ("\"edge-2\"", record->as_object().lookup("host")->string());
    EXPECT_EQ("91", record->as_object().lookup("_value")->string());
}

TEST(RuntimeEvalTest, EvaluatesCsvFromRawStringPackageBuiltin) {
    Environment env;
    auto csv_or = BuiltinRegistry::ImportPackage("csv");
    ASSERT_TRUE(csv_or.ok()) << csv_or.status();
    env.define("csv", *csv_or);
    const auto& expr = ParseAssignmentInit(
        "result = csv.from(csv: \"_time,_measurement,_value\\n"
        "2024-01-01T00:00:00Z,cpu,95.5\\n"
        "2024-01-01T00:01:00Z,cpu,80.0\\n\", mode: \"raw\")");

    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(2, result->as_table().rows.size());
    ASSERT_NE(nullptr, result->as_table().rows[0]);
    EXPECT_EQ("\"2024-01-01T00:00:00Z\"",
              result->as_table().rows[0]->lookup("_time")->string());
    EXPECT_EQ("\"cpu\"", result->as_table().rows[0]->lookup("_measurement")->string());
    EXPECT_EQ("\"95.5\"", result->as_table().rows[0]->lookup("_value")->string());
}

TEST(RuntimeEvalTest, EvaluatesArrayFromPackageBuiltin) {
    Environment env;
    auto array_or = BuiltinRegistry::ImportPackage("array");
    ASSERT_TRUE(array_or.ok()) << array_or.status();
    env.define("array", *array_or);
    const auto& expr = ParseAssignmentInit(R"(
        result = array.from(
            rows: [
                {_time: "2024-01-01T00:00:00Z", host: "edge-1", _value: 70},
                {_time: "2024-01-01T00:01:00Z", host: "edge-2", _value: 91},
            ],
        )
    )");

    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    EXPECT_EQ("array", result->as_table().bucket);
    ASSERT_EQ(2, result->as_table().rows.size());
    ASSERT_NE(nullptr, result->as_table().rows[0]);
    EXPECT_EQ("\"edge-1\"", result->as_table().rows[0]->lookup("host")->string());
    EXPECT_EQ("70", result->as_table().rows[0]->lookup("_value")->string());
}

TEST(RuntimeEvalTest, EvaluatesArrayPackageHelpers) {
    Environment env;
    auto array_or = BuiltinRegistry::ImportPackage("array");
    ASSERT_TRUE(array_or.ok()) << array_or.status();
    env.define("array", *array_or);

    const auto& concat_expr = ParseAssignmentInit(R"(
        result = array.concat(arr: [1, 2], v: [3, 4])
    )");
    auto concat = ExpressionEvaluator::Evaluate(concat_expr, env);
    ASSERT_TRUE(concat.ok()) << concat.status();
    EXPECT_EQ("[1, 2, 3, 4]", concat->string());

    const auto& filter_expr = ParseAssignmentInit(R"(
        result = array.filter(arr: [1, 2, 3, 4], fn: (x) => x > 2)
    )");
    auto filter = ExpressionEvaluator::Evaluate(filter_expr, env);
    ASSERT_TRUE(filter.ok()) << filter.status();
    EXPECT_EQ("[3, 4]", filter->string());

    const auto& map_expr = ParseAssignmentInit(R"(
        result = array.map(arr: [1, 2, 3], fn: (x) => ({_value: x * 10, label: "v${x}"}))
    )");
    auto map = ExpressionEvaluator::Evaluate(map_expr, env);
    ASSERT_TRUE(map.ok()) << map.status();
    EXPECT_EQ(
        "[{_value: 10, label: \"v1\"}, {_value: 20, label: \"v2\"}, {_value: 30, label: \"v3\"}]",
        map->string());
}

TEST(RuntimeEvalTest, ReportsArrayPackageHelperErrors) {
    Environment env;
    auto array_or = BuiltinRegistry::ImportPackage("array");
    ASSERT_TRUE(array_or.ok()) << array_or.status();
    env.define("array", *array_or);

    const auto& missing_array_expr = ParseAssignmentInit(R"(
        result = array.concat(arr: [1], v: 2)
    )");
    auto missing_array = ExpressionEvaluator::Evaluate(missing_array_expr, env);
    ASSERT_FALSE(missing_array.ok());
    EXPECT_EQ(absl::StatusCode::kInvalidArgument, missing_array.status().code());

    const auto& invalid_filter_expr = ParseAssignmentInit(R"(
        result = array.filter(arr: [1, 2], fn: (x) => x + 1)
    )");
    auto invalid_filter = ExpressionEvaluator::Evaluate(invalid_filter_expr, env);
    ASSERT_FALSE(invalid_filter.ok());
    EXPECT_EQ(absl::StatusCode::kInvalidArgument, invalid_filter.status().code());
}

TEST(RuntimeEvalTest, EvaluatesCsvFromRawFilePackageBuiltin) {
    const std::string path = "/tmp/flux_runtime_csv_from_file_test.csv";
    {
        std::ofstream file(path);
        file << "_time,_measurement,_value\n";
        file << "2024-01-01T00:00:00Z,cpu,95.5\n";
    }

    Environment env;
    auto csv_or = BuiltinRegistry::ImportPackage("csv");
    ASSERT_TRUE(csv_or.ok()) << csv_or.status();
    env.define("csv", *csv_or);
    const auto& expr =
        ParseAssignmentInit("result = csv.from(file: \"" + path + "\", mode: \"raw\")");

    auto result = ExpressionEvaluator::Evaluate(expr, env);

    std::remove(path.c_str());
    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(1, result->as_table().rows.size());
    ASSERT_NE(nullptr, result->as_table().rows[0]);
    EXPECT_EQ("\"95.5\"", result->as_table().rows[0]->lookup("_value")->string());
}

TEST(RuntimeEvalTest, EvaluatesCsvFromAnnotatedStringPackageBuiltin) {
    Environment env;
    auto csv_or = BuiltinRegistry::ImportPackage("csv");
    ASSERT_TRUE(csv_or.ok()) << csv_or.status();
    env.define("csv", *csv_or);
    const auto& expr = ParseAssignmentInit(
        "result = csv.from(csv: \"#datatype,string,long,dateTime:RFC3339,string,double,boolean\\n"
        "#group,false,false,true,true,false,false\\n"
        "#default,_result,,,,,\\n"
        ",result,table,_time,_measurement,_value,active\\n"
        ",,0,2024-01-01T00:00:00Z,cpu,95.5,true\\n\")");

    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(1, result->as_table().rows.size());
    ASSERT_NE(nullptr, result->as_table().rows[0]);
    const auto& row = result->as_table().rows[0];
    EXPECT_EQ("\"_result\"", row->lookup("result")->string());
    EXPECT_EQ("0", row->lookup("table")->string());
    EXPECT_EQ("2024-01-01T00:00:00Z", row->lookup("_time")->string());
    EXPECT_EQ("\"cpu\"", row->lookup("_measurement")->string());
    EXPECT_EQ("95.5", row->lookup("_value")->string());
    EXPECT_EQ("true", row->lookup("active")->string());
    ASSERT_NE(nullptr, row->lookup("_group"));
    ASSERT_EQ(Value::Type::Object, row->lookup("_group")->type());
    ASSERT_NE(nullptr, row->lookup("_group")->as_object().lookup("_measurement"));
    EXPECT_EQ("\"cpu\"", row->lookup("_group")->as_object().lookup("_measurement")->string());
}

TEST(RuntimeEvalTest, EvaluatesCsvFromAnnotatedStringWithMultipleBlocks) {
    Environment env;
    auto csv_or = BuiltinRegistry::ImportPackage("csv");
    ASSERT_TRUE(csv_or.ok()) << csv_or.status();
    env.define("csv", *csv_or);
    const auto& expr = ParseAssignmentInit(
        "result = csv.from(csv: \"#datatype,string,long,dateTime:RFC3339,string,double\\n"
        "#group,false,false,true,true,false\\n"
        "#default,_result,,,,\\n"
        ",result,table,_time,_measurement,_value\\n"
        ",cpu,0,2024-01-01T00:00:00Z,cpu,95.5\\n"
        "\\n"
        "#datatype,string,long,dateTime:RFC3339,string,double\\n"
        "#group,false,false,true,true,false\\n"
        "#default,_result,,,,\\n"
        ",result,table,_time,_measurement,_value\\n"
        ",mem,1,2024-01-01T00:01:00Z,mem,42.0\\n\")");

    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(2, result->as_table().rows.size());
    ASSERT_NE(nullptr, result->as_table().rows[0]);
    ASSERT_NE(nullptr, result->as_table().rows[1]);
    EXPECT_EQ("\"cpu\"", result->as_table().rows[0]->lookup("result")->string());
    EXPECT_EQ("0", result->as_table().rows[0]->lookup("table")->string());
    EXPECT_EQ("\"mem\"", result->as_table().rows[1]->lookup("result")->string());
    EXPECT_EQ("1", result->as_table().rows[1]->lookup("table")->string());
}

TEST(RuntimeEvalTest, ReportsCsvFromArgumentAndAnnotationErrors) {
    Environment env;
    auto csv_or = BuiltinRegistry::ImportPackage("csv");
    ASSERT_TRUE(csv_or.ok()) << csv_or.status();
    env.define("csv", *csv_or);

    const auto& invalid_mode =
        ParseAssignmentInit("result = csv.from(csv: \"a\\n1\\n\", mode: \"stream\")");
    auto invalid_mode_result = ExpressionEvaluator::Evaluate(invalid_mode, env);
    ASSERT_FALSE(invalid_mode_result.ok());
    EXPECT_EQ(absl::StatusCode::kInvalidArgument, invalid_mode_result.status().code());

    const auto& both_sources =
        ParseAssignmentInit("result = csv.from(csv: \"a\\n1\\n\", file: \"data.csv\")");
    auto both_sources_result = ExpressionEvaluator::Evaluate(both_sources, env);
    ASSERT_FALSE(both_sources_result.ok());
    EXPECT_EQ(absl::StatusCode::kInvalidArgument, both_sources_result.status().code());

    const auto& missing_group = ParseAssignmentInit(
        "result = csv.from(csv: \"#datatype,string,long\\n"
        ",name,value\\n"
        ",cpu,1\\n\")");
    auto missing_group_result = ExpressionEvaluator::Evaluate(missing_group, env);
    ASSERT_FALSE(missing_group_result.ok());
    EXPECT_EQ(absl::StatusCode::kInvalidArgument, missing_group_result.status().code());

    const auto& invalid_typed_value = ParseAssignmentInit(
        "result = csv.from(csv: \"#datatype,string,long\\n"
        "#group,false,false\\n"
        ",name,value\\n"
        ",cpu,not_int\\n\")");
    auto invalid_typed_value_result = ExpressionEvaluator::Evaluate(invalid_typed_value, env);
    ASSERT_FALSE(invalid_typed_value_result.ok());
    EXPECT_EQ(absl::StatusCode::kInvalidArgument, invalid_typed_value_result.status().code());
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

TEST(RuntimeEvalTest, EvaluatesRangeUsingStopExclusiveAndRfc3339Instants) {
    Environment env;
    BuiltinRegistry::Install(env);
    const auto& expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_time: "2023-12-31T23:00:30-01:00", _value: 1.0, host: "inside"},
                {_time: "2024-01-01T00:00:00Z", _value: 2.0, host: "start"},
                {_time: "2024-01-01T00:01:00Z", _value: 3.0, host: "stop"},
            ],
        )
            |> range(start: 2024-01-01T00:00:00Z, stop: 2024-01-01T00:01:00Z)
    )");

    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(2, result->as_table().rows.size());
    EXPECT_EQ("\"inside\"", result->as_table().rows[0]->lookup("host")->string());
    EXPECT_EQ("\"start\"", result->as_table().rows[1]->lookup("host")->string());
    EXPECT_EQ("2024-01-01T00:00:00Z", *result->as_table().range_start);
    EXPECT_EQ("2024-01-01T00:01:00Z", *result->as_table().range_stop);
}

TEST(RuntimeEvalTest, EvaluatesReduceKeepDropAndLimitBuiltins) {
    Environment env;
    BuiltinRegistry::Install(env);
    const auto& expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_measurement: "cpu", _value: 95.0, host: "a"},
                {_measurement: "cpu", _value: 70.0, host: "b"},
                {_measurement: "mem", _value: 40.0, host: "c"},
            ],
        )
            |> limit(n: 2)
            |> keep(columns: ["_measurement", "_value"])
            |> reduce(
                identity: {count: 0, total: 0.0},
                fn: (r, accumulator) => ({
                    count: accumulator.count + 1,
                    total: accumulator.total + r._value,
                }),
            )
            |> drop(columns: ["count"])
    )");

    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(1, result->as_table().rows.size());
    ASSERT_NE(nullptr, result->as_table().rows[0]);
    EXPECT_EQ(nullptr, result->as_table().rows[0]->lookup("count"));
    ASSERT_NE(nullptr, result->as_table().rows[0]->lookup("total"));
    EXPECT_EQ("165", result->as_table().rows[0]->lookup("total")->string());
}

TEST(RuntimeEvalTest, EvaluatesRenameDuplicateAndSetBuiltins) {
    Environment env;
    BuiltinRegistry::Install(env);
    const auto& expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_measurement: "cpu", _value: 95.0, host: "a"},
                {_measurement: "mem", _value: 40.0, host: "b"},
            ],
        )
            |> duplicate(column: "_value", as: "raw_value")
            |> rename(columns: {_measurement: "measurement", _value: "usage"})
            |> set(key: "env", value: "prod")
    )");

    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(2, result->as_table().rows.size());
    ASSERT_NE(nullptr, result->as_table().rows[0]);
    EXPECT_EQ(nullptr, result->as_table().rows[0]->lookup("_measurement"));
    EXPECT_EQ(nullptr, result->as_table().rows[0]->lookup("_value"));
    EXPECT_EQ("\"cpu\"", result->as_table().rows[0]->lookup("measurement")->string());
    EXPECT_EQ("95", result->as_table().rows[0]->lookup("usage")->string());
    EXPECT_EQ("95", result->as_table().rows[0]->lookup("raw_value")->string());
    EXPECT_EQ("\"prod\"", result->as_table().rows[0]->lookup("env")->string());
    ASSERT_NE(nullptr, result->as_table().rows[1]);
    EXPECT_EQ("\"mem\"", result->as_table().rows[1]->lookup("measurement")->string());
    EXPECT_EQ("40", result->as_table().rows[1]->lookup("usage")->string());
}

TEST(RuntimeEvalTest, EvaluatesSortGroupCountFirstAndLastBuiltins) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& sorted_expr = ParseAssignmentInit(R"(
        result = from(
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
    )");
    auto sorted = ExpressionEvaluator::Evaluate(sorted_expr, env);
    ASSERT_TRUE(sorted.ok()) << sorted.status();
    ASSERT_EQ(Value::Type::Table, sorted->type());
    ASSERT_EQ(2, sorted->as_table().rows.size());
    ASSERT_EQ(2, sorted->as_table().table_count());
    ASSERT_NE(nullptr, sorted->as_table().rows[0]);
    EXPECT_EQ("95", sorted->as_table().rows[0]->lookup("_value")->string());
    ASSERT_NE(nullptr, sorted->as_table().rows[0]->lookup("_group"));
    EXPECT_EQ("{_measurement: \"cpu\"}", sorted->as_table().rows[0]->lookup("_group")->string());
    ASSERT_NE(nullptr, sorted->as_table().rows[1]);
    EXPECT_EQ("40", sorted->as_table().rows[1]->lookup("_value")->string());
    EXPECT_EQ("{_measurement: \"mem\"}", sorted->as_table().rows[1]->lookup("_group")->string());

    const auto& except_expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_measurement: "cpu", host: "a", _value: 70.0},
                {_measurement: "cpu", host: "a", _value: 95.0},
                {_measurement: "cpu", host: "b", _value: 40.0},
            ],
        )
            |> group(columns: ["_value"], mode: "except")
            |> count(column: "_value")
    )");
    auto except_grouped = ExpressionEvaluator::Evaluate(except_expr, env);
    ASSERT_TRUE(except_grouped.ok()) << except_grouped.status();
    ASSERT_EQ(2, except_grouped->as_table().rows.size());
    ASSERT_EQ(2, except_grouped->as_table().table_count());
    EXPECT_EQ("\"a\"", except_grouped->as_table().rows[0]->lookup("host")->string());
    EXPECT_EQ("2", except_grouped->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("\"b\"", except_grouped->as_table().rows[1]->lookup("host")->string());
    EXPECT_EQ("1", except_grouped->as_table().rows[1]->lookup("_value")->string());

    const auto& count_expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_measurement: "cpu", host: "a", _value: 70.0},
                {_measurement: "cpu"},
                {_measurement: "mem", host: "b", _value: 40.0},
            ],
        )
            |> group(columns: ["_measurement"])
            |> count(column: "_value")
    )");
    auto count = ExpressionEvaluator::Evaluate(count_expr, env);
    ASSERT_TRUE(count.ok()) << count.status();
    ASSERT_EQ(Value::Type::Table, count->type());
    ASSERT_EQ(2, count->as_table().rows.size());
    ASSERT_EQ(2, count->as_table().table_count());
    EXPECT_EQ("\"cpu\"", count->as_table().rows[0]->lookup("_measurement")->string());
    EXPECT_EQ("1", count->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("\"mem\"", count->as_table().rows[1]->lookup("_measurement")->string());
    EXPECT_EQ("1", count->as_table().rows[1]->lookup("_value")->string());

    const auto& last_expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {host: "a", _value: 1},
                {host: "a", _value: 2},
                {host: "b", _value: 3},
            ],
        )
            |> group(columns: ["host"])
            |> last()
    )");
    auto last = ExpressionEvaluator::Evaluate(last_expr, env);
    ASSERT_TRUE(last.ok()) << last.status();
    ASSERT_EQ(2, last->as_table().rows.size());
    ASSERT_EQ(2, last->as_table().table_count());
    EXPECT_EQ("2", last->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("3", last->as_table().rows[1]->lookup("_value")->string());
}

TEST(RuntimeEvalTest, FilterDropsEmptyLogicalTablesByDefault) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {host: "edge-1", _value: 95.0},
                {host: "edge-2", _value: 60.0},
                {host: "edge-3", _value: 85.0},
            ],
        )
            |> group(columns: ["host"])
            |> filter(fn: (r) => r._value >= 80.0)
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(2, result->as_table().rows.size());
    ASSERT_EQ(2, result->as_table().table_count());
    EXPECT_EQ("\"edge-1\"", result->as_table().rows[0]->lookup("host")->string());
    EXPECT_EQ("\"edge-3\"", result->as_table().rows[1]->lookup("host")->string());
}

TEST(RuntimeEvalTest, FilterCanKeepEmptyLogicalTablesExplicitly) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {host: "edge-1", _value: 95.0},
                {host: "edge-2", _value: 60.0},
                {host: "edge-3", _value: 85.0},
            ],
        )
            |> group(columns: ["host"])
            |> filter(fn: (r) => r._value >= 80.0, onEmpty: "keep")
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(2, result->as_table().rows.size());
    ASSERT_EQ(3, result->as_table().table_count());
    ASSERT_EQ(3, result->as_table().tables.size());
    EXPECT_TRUE(result->as_table().tables[1].rows.empty());
    ASSERT_NE(nullptr, result->as_table().tables[1].group_key);
    EXPECT_EQ("\"edge-2\"",
              result->as_table().tables[1].group_key->lookup("host")->string());
}

TEST(RuntimeEvalTest, CountPreservesKeptEmptyLogicalTablesAsZeroRows) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {host: "edge-1", _value: 95.0},
                {host: "edge-2", _value: 60.0},
                {host: "edge-3", _value: 85.0},
            ],
        )
            |> group(columns: ["host"])
            |> filter(fn: (r) => r._value >= 80.0, onEmpty: "keep")
            |> count(column: "_value")
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(3, result->as_table().rows.size());
    ASSERT_EQ(3, result->as_table().table_count());
    EXPECT_EQ("\"edge-1\"", result->as_table().rows[0]->lookup("host")->string());
    EXPECT_EQ("1", result->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("\"edge-2\"", result->as_table().rows[1]->lookup("host")->string());
    EXPECT_EQ("0", result->as_table().rows[1]->lookup("_value")->string());
    EXPECT_EQ("{host: \"edge-2\"}", result->as_table().rows[1]->lookup("_group")->string());
    EXPECT_EQ("\"edge-3\"", result->as_table().rows[2]->lookup("host")->string());
    EXPECT_EQ("1", result->as_table().rows[2]->lookup("_value")->string());
}

TEST(RuntimeEvalTest, FirstAndLastDropKeptEmptyLogicalTablesAndSkipNullValues) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& first_expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {host: "edge-1", sample: "ignored"},
                {host: "edge-1", _value: 95.0, sample: "first"},
                {host: "edge-2", _value: 60.0, sample: "drop-me"},
                {host: "edge-3", sample: "ignored-too"},
                {host: "edge-3", _value: 85.0, sample: "picked"},
            ],
        )
            |> group(columns: ["host"])
            |> filter(fn: (r) => not exists r._value or r._value >= 80.0, onEmpty: "keep")
            |> first()
    )");
    auto first = ExpressionEvaluator::Evaluate(first_expr, env);

    ASSERT_TRUE(first.ok()) << first.status();
    ASSERT_EQ(Value::Type::Table, first->type());
    ASSERT_EQ(2, first->as_table().rows.size());
    ASSERT_EQ(2, first->as_table().table_count());
    EXPECT_EQ("\"edge-1\"", first->as_table().rows[0]->lookup("host")->string());
    EXPECT_EQ("\"first\"", first->as_table().rows[0]->lookup("sample")->string());
    EXPECT_EQ("\"edge-3\"", first->as_table().rows[1]->lookup("host")->string());
    EXPECT_EQ("\"picked\"", first->as_table().rows[1]->lookup("sample")->string());

    const auto& last_expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {host: "edge-1", _value: 90.0, sample: "keep"},
                {host: "edge-1", sample: "ignored-last-null"},
                {host: "edge-2", _value: 60.0, sample: "drop-me"},
                {host: "edge-3", _value: 81.0, sample: "older"},
                {host: "edge-3", _value: 82.0, sample: "picked"},
                {host: "edge-3", sample: "ignored-tail-null"},
            ],
        )
            |> group(columns: ["host"])
            |> filter(fn: (r) => not exists r._value or r._value >= 80.0, onEmpty: "keep")
            |> last()
    )");
    auto last = ExpressionEvaluator::Evaluate(last_expr, env);

    ASSERT_TRUE(last.ok()) << last.status();
    ASSERT_EQ(Value::Type::Table, last->type());
    ASSERT_EQ(2, last->as_table().rows.size());
    ASSERT_EQ(2, last->as_table().table_count());
    EXPECT_EQ("\"edge-1\"", last->as_table().rows[0]->lookup("host")->string());
    EXPECT_EQ("\"keep\"", last->as_table().rows[0]->lookup("sample")->string());
    EXPECT_EQ("\"edge-3\"", last->as_table().rows[1]->lookup("host")->string());
    EXPECT_EQ("\"picked\"", last->as_table().rows[1]->lookup("sample")->string());
}

TEST(RuntimeEvalTest, EvaluatesUnionAndJoinBuiltins) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& union_expr = ParseAssignmentInit(R"(
        result = union(tables: [
            from(bucket: "cpu", rows: [{_time: "t1", _value: 90.0}]),
            from(bucket: "mem", rows: [{_time: "t1", _value: 40.0}, {_time: "t2", _value: 50.0}]),
        ])
    )");
    auto union_result = ExpressionEvaluator::Evaluate(union_expr, env);
    ASSERT_TRUE(union_result.ok()) << union_result.status();
    ASSERT_EQ(Value::Type::Table, union_result->type());
    EXPECT_EQ(3, union_result->as_table().rows.size());

    const auto& join_expr = ParseAssignmentInit(R"(
        result = join(
            tables: {
                cpu: from(bucket: "cpu", rows: [
                    {_time: "t1", _value: 90.0, host: "a"},
                    {_time: "t2", _value: 70.0, host: "b"},
                ]),
                mem: from(bucket: "mem", rows: [
                    {_time: "t1", _value: 40.0},
                    {_time: "t3", _value: 20.0},
                ]),
            },
            on: ["_time"],
        )
    )");
    auto join_result = ExpressionEvaluator::Evaluate(join_expr, env);
    ASSERT_TRUE(join_result.ok()) << join_result.status();
    ASSERT_EQ(Value::Type::Table, join_result->type());
    ASSERT_EQ(1, join_result->as_table().rows.size());
    ASSERT_NE(nullptr, join_result->as_table().rows[0]);
    EXPECT_EQ("\"t1\"", join_result->as_table().rows[0]->lookup("_time")->string());
    EXPECT_EQ("90", join_result->as_table().rows[0]->lookup("_value_cpu")->string());
    EXPECT_EQ("40", join_result->as_table().rows[0]->lookup("_value_mem")->string());
    EXPECT_EQ("\"a\"", join_result->as_table().rows[0]->lookup("host")->string());
}

TEST(RuntimeEvalTest, EvaluatesJoinAgainstMatchingGroupKeyTablesOnly) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& expr = ParseAssignmentInit(R"(
        result = join(
            tables: {
                cpu: from(bucket: "cpu", rows: [
                    {_time: "t1", host: "a", region: "east", _value: 90.0},
                    {_time: "t2", host: "a", region: "east", _value: 91.0},
                    {_time: "t3", host: "b", region: "west", _value: 70.0},
                    {host: "a", region: "east", _value: 999.0},
                ])
                    |> group(columns: ["host"]),
                mem: from(bucket: "mem", rows: [
                    {_time: "t1", host: "a", region: "east", _value: 40.0},
                    {_time: "t2", host: "a", region: "east", _value: 41.0},
                    {_time: "t4", host: "c", region: "north", _value: 20.0},
                    {host: "a", region: "east", _value: 111.0},
                ])
                    |> group(columns: ["host"]),
            },
            method: "inner",
            on: ["_time"],
        )
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(2, result->as_table().rows.size());
    ASSERT_EQ(1, result->as_table().table_count());
    ASSERT_NE(nullptr, result->as_table().rows[0]);
    ASSERT_NE(nullptr, result->as_table().rows[1]);
    EXPECT_EQ("\"t1\"", result->as_table().rows[0]->lookup("_time")->string());
    EXPECT_EQ("\"t2\"", result->as_table().rows[1]->lookup("_time")->string());
    EXPECT_EQ("90", result->as_table().rows[0]->lookup("_value_cpu")->string());
    EXPECT_EQ("40", result->as_table().rows[0]->lookup("_value_mem")->string());
    EXPECT_EQ("\"a\"", result->as_table().rows[0]->lookup("host_cpu")->string());
    EXPECT_EQ("\"a\"", result->as_table().rows[0]->lookup("host_mem")->string());
    EXPECT_EQ("\"east\"", result->as_table().rows[0]->lookup("region_cpu")->string());
    EXPECT_EQ("\"east\"", result->as_table().rows[0]->lookup("region_mem")->string());
    const Value* group = result->as_table().rows[0]->lookup("_group");
    ASSERT_NE(nullptr, group);
    EXPECT_EQ("{host_cpu: \"a\", host_mem: \"a\"}", group->string());
}

TEST(RuntimeEvalTest, EvaluatesDistinctBuiltin) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_time: "2024-01-01T00:00:10Z", host: "a", region: "east", _value: 10.0},
                {_time: "2024-01-01T00:00:40Z", host: "a", region: "east", _value: 20.0},
                {_time: "2024-01-01T00:01:10Z", host: "b", region: "west", _value: 30.0},
            ],
        )
            |> distinct(column: "host")
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(2, result->as_table().rows.size());
    EXPECT_EQ("\"a\"", result->as_table().rows[0]->lookup("host")->string());
    EXPECT_EQ("10", result->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("\"b\"", result->as_table().rows[1]->lookup("host")->string());
    EXPECT_EQ("30", result->as_table().rows[1]->lookup("_value")->string());
}

TEST(RuntimeEvalTest, EvaluatesTailBuiltinWithOffset) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_time: "2024-01-01T00:00:10Z", host: "a", _value: 10.0},
                {_time: "2024-01-01T00:00:20Z", host: "b", _value: 20.0},
                {_time: "2024-01-01T00:00:30Z", host: "c", _value: 30.0},
                {_time: "2024-01-01T00:00:40Z", host: "d", _value: 40.0},
            ],
        )
            |> tail(n: 2, offset: 1)
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(2, result->as_table().rows.size());
    ASSERT_NE(nullptr, result->as_table().rows[0]);
    ASSERT_NE(nullptr, result->as_table().rows[1]);
    EXPECT_EQ("\"b\"", result->as_table().rows[0]->lookup("host")->string());
    EXPECT_EQ("20", result->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("\"c\"", result->as_table().rows[1]->lookup("host")->string());
    EXPECT_EQ("30", result->as_table().rows[1]->lookup("_value")->string());
}

TEST(RuntimeEvalTest, PreservesLogicalTablesAcrossRowTransformBuiltins) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& limit_expr = ParseAssignmentInit(R"(
        result = union(
            tables: [
                from(bucket: "telegraf", rows: [{host: "a", _value: 1.0}, {host: "a", _value: 2.0}])
                    |> group(columns: ["host"]),
                from(bucket: "telegraf", rows: [{host: "b", _value: 3.0}, {host: "b", _value: 4.0}])
                    |> group(columns: ["host"]),
            ],
        )
            |> limit(n: 1)
    )");
    auto limit_result = ExpressionEvaluator::Evaluate(limit_expr, env);
    ASSERT_TRUE(limit_result.ok()) << limit_result.status();
    ASSERT_EQ(2, limit_result->as_table().table_count());
    ASSERT_EQ(2, limit_result->as_table().rows.size());
    EXPECT_EQ("\"a\"", limit_result->as_table().tables[0].rows[0]->lookup("host")->string());
    EXPECT_EQ("\"b\"", limit_result->as_table().tables[1].rows[0]->lookup("host")->string());

    const auto& map_expr = ParseAssignmentInit(R"(
        result = union(
            tables: [
                from(bucket: "telegraf", rows: [{host: "a", _value: 1.0}, {host: "a", _value: 2.0}])
                    |> group(columns: ["host"]),
                from(bucket: "telegraf", rows: [{host: "b", _value: 3.0}, {host: "b", _value: 4.0}])
                    |> group(columns: ["host"]),
            ],
        )
            |> map(fn: (r) => ({r with seen: true}))
    )");
    auto map_result = ExpressionEvaluator::Evaluate(map_expr, env);
    ASSERT_TRUE(map_result.ok()) << map_result.status();
    ASSERT_EQ(2, map_result->as_table().table_count());
    EXPECT_EQ(2, map_result->as_table().tables[0].rows.size());
    EXPECT_EQ(2, map_result->as_table().tables[1].rows.size());
    EXPECT_EQ("true", map_result->as_table().tables[0].rows[0]->lookup("seen")->string());

    const auto& keep_expr = ParseAssignmentInit(R"(
        result = union(
            tables: [
                from(bucket: "telegraf", rows: [{host: "a", _value: 1.0}, {host: "a", _value: 2.0}])
                    |> group(columns: ["host"]),
                from(bucket: "telegraf", rows: [{host: "b", _value: 3.0}, {host: "b", _value: 4.0}])
                    |> group(columns: ["host"]),
            ],
        )
            |> keep(columns: ["host", "_value"])
    )");
    auto keep_result = ExpressionEvaluator::Evaluate(keep_expr, env);
    ASSERT_TRUE(keep_result.ok()) << keep_result.status();
    ASSERT_EQ(2, keep_result->as_table().table_count());
    EXPECT_EQ(2, keep_result->as_table().tables[0].rows.size());
    EXPECT_EQ(2, keep_result->as_table().tables[1].rows.size());
}

TEST(RuntimeEvalTest, EvaluatesReducePerLogicalTable) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& expr = ParseAssignmentInit(R"(
        result = union(
            tables: [
                from(bucket: "telegraf", rows: [{host: "a", _value: 1.0}, {host: "a", _value: 2.0}])
                    |> group(columns: ["host"]),
                from(bucket: "telegraf", rows: [{host: "b", _value: 3.0}, {host: "b", _value: 4.0}])
                    |> group(columns: ["host"]),
            ],
        )
            |> reduce(
                identity: {count: 0, total: 0.0},
                fn: (r, accumulator) => ({
                    count: accumulator.count + 1,
                    total: accumulator.total + r._value,
                }),
            )
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(2, result->as_table().table_count());
    ASSERT_EQ(2, result->as_table().rows.size());
    EXPECT_EQ("2", result->as_table().tables[0].rows[0]->lookup("count")->string());
    EXPECT_EQ("3", result->as_table().tables[0].rows[0]->lookup("total")->string());
    EXPECT_EQ("2", result->as_table().tables[1].rows[0]->lookup("count")->string());
    EXPECT_EQ("7", result->as_table().tables[1].rows[0]->lookup("total")->string());
}

TEST(RuntimeEvalTest, EvaluatesPivotBuiltin) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_time: 2024-01-01T00:01:00Z, host: "a", metric: "cpu", usage: 72.0},
                {_time: 2024-01-01T00:01:00Z, host: "a", metric: "mem", usage: 63.0},
                {_time: 2024-01-01T00:02:00Z, host: "a", metric: "cpu", usage: 82.0},
                {_time: 2024-01-01T00:02:00Z, host: "a", metric: "mem", usage: 68.0},
            ],
        )
            |> pivot(rowKey: ["_time", "host"], columnKey: ["metric"], valueColumn: "usage")
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(2, result->as_table().rows.size());
    ASSERT_NE(nullptr, result->as_table().rows[0]);
    ASSERT_NE(nullptr, result->as_table().rows[1]);
    EXPECT_EQ("2024-01-01T00:01:00Z", result->as_table().rows[0]->lookup("_time")->string());
    EXPECT_EQ("\"a\"", result->as_table().rows[0]->lookup("host")->string());
    EXPECT_EQ("72", result->as_table().rows[0]->lookup("cpu")->string());
    EXPECT_EQ("63", result->as_table().rows[0]->lookup("mem")->string());
    EXPECT_EQ("2024-01-01T00:02:00Z", result->as_table().rows[1]->lookup("_time")->string());
    EXPECT_EQ("82", result->as_table().rows[1]->lookup("cpu")->string());
    EXPECT_EQ("68", result->as_table().rows[1]->lookup("mem")->string());
}

TEST(RuntimeEvalTest, EvaluatesPivotPerLogicalTableWithoutCrossChunkMerge) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_time: 2024-01-01T00:01:00Z, host: "a", metric: "cpu", usage: 72.0},
                {_time: 2024-01-01T00:01:00Z, host: "a", metric: "mem", usage: 63.0},
                {_time: 2024-01-01T00:01:00Z, host: "b", metric: "cpu", usage: 82.0},
                {_time: 2024-01-01T00:01:00Z, host: "b", metric: "mem", usage: 68.0},
            ],
        )
            |> group(columns: ["host"])
            |> pivot(rowKey: ["_time"], columnKey: ["metric"], valueColumn: "usage")
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(2, result->as_table().table_count());
    ASSERT_EQ(2, result->as_table().rows.size());
    ASSERT_EQ(1, result->as_table().tables[0].rows.size());
    ASSERT_EQ(1, result->as_table().tables[1].rows.size());
    EXPECT_EQ("\"a\"", result->as_table().tables[0].rows[0]->lookup("host")->string());
    EXPECT_EQ("72", result->as_table().tables[0].rows[0]->lookup("cpu")->string());
    EXPECT_EQ("63", result->as_table().tables[0].rows[0]->lookup("mem")->string());
    EXPECT_EQ("\"b\"", result->as_table().tables[1].rows[0]->lookup("host")->string());
    EXPECT_EQ("82", result->as_table().tables[1].rows[0]->lookup("cpu")->string());
    EXPECT_EQ("68", result->as_table().tables[1].rows[0]->lookup("mem")->string());
}

TEST(RuntimeEvalTest, EvaluatesFillBuiltinUsePreviousAndValue) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_time: 2024-01-01T00:00:00Z, host: "a", _value: 10.0},
                {_time: 2024-01-01T00:01:00Z, host: "a"},
                {_time: 2024-01-01T00:02:00Z, host: "a", _value: 30.0},
                {_time: 2024-01-01T00:03:00Z, host: "b"},
            ],
        )
            |> group(columns: ["host"])
            |> fill(usePrevious: true)
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(4, result->as_table().rows.size());
    EXPECT_EQ("10", result->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("10", result->as_table().rows[1]->lookup("_value")->string());
    EXPECT_EQ("30", result->as_table().rows[2]->lookup("_value")->string());
    EXPECT_EQ(nullptr, result->as_table().rows[3]->lookup("_value"));

    const auto& value_expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_time: 2024-01-01T00:00:00Z, host: "a"},
                {_time: 2024-01-01T00:01:00Z, host: "a", usage: 2.0},
            ],
        )
            |> fill(column: "usage", value: 0.0)
    )");
    auto value_result = ExpressionEvaluator::Evaluate(value_expr, env);

    ASSERT_TRUE(value_result.ok()) << value_result.status();
    ASSERT_EQ(2, value_result->as_table().rows.size());
    EXPECT_EQ("0", value_result->as_table().rows[0]->lookup("usage")->string());
    EXPECT_EQ("2", value_result->as_table().rows[1]->lookup("usage")->string());
}

TEST(RuntimeEvalTest, EvaluatesElapsedBuiltin) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_time: 2024-01-01T00:00:00Z, host: "a", _value: 10.0},
                {_time: 2024-01-01T00:00:10Z, host: "b", _value: 20.0},
                {_time: 2024-01-01T00:00:30Z, host: "a", _value: 30.0},
                {_time: 2024-01-01T00:01:00Z, host: "b", _value: 40.0},
            ],
        )
            |> group(columns: ["host"])
            |> elapsed(unit: 10s)
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(2, result->as_table().rows.size());
    ASSERT_NE(nullptr, result->as_table().rows[0]);
    ASSERT_NE(nullptr, result->as_table().rows[1]);
    EXPECT_EQ("\"a\"", result->as_table().rows[0]->lookup("host")->string());
    EXPECT_EQ("3", result->as_table().rows[0]->lookup("elapsed")->string());
    EXPECT_EQ("{host: \"a\"}", result->as_table().rows[0]->lookup("_group")->string());
    EXPECT_EQ("\"b\"", result->as_table().rows[1]->lookup("host")->string());
    EXPECT_EQ("5", result->as_table().rows[1]->lookup("elapsed")->string());
}

TEST(RuntimeEvalTest, EvaluatesDifferenceBuiltin) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_time: 2024-01-01T00:00:00Z, host: "a", _value: 10.0},
                {_time: 2024-01-01T00:00:10Z, host: "b", _value: 20.0},
                {_time: 2024-01-01T00:00:30Z, host: "a", _value: 17.0},
                {_time: 2024-01-01T00:01:00Z, host: "b", _value: 15.0},
            ],
        )
            |> group(columns: ["host"])
            |> difference()
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(2, result->as_table().rows.size());
    ASSERT_NE(nullptr, result->as_table().rows[0]);
    ASSERT_NE(nullptr, result->as_table().rows[1]);
    EXPECT_EQ("\"a\"", result->as_table().rows[0]->lookup("host")->string());
    EXPECT_EQ("7", result->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("\"b\"", result->as_table().rows[1]->lookup("host")->string());
    EXPECT_EQ("-5", result->as_table().rows[1]->lookup("_value")->string());
}

TEST(RuntimeEvalTest, EvaluatesDifferenceBuiltinWithNonNegativeAndKeepFirst) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_time: 2024-01-01T00:00:00Z, host: "a", _value: 10.0},
                {_time: 2024-01-01T00:00:20Z, host: "a", _value: 7.0},
                {_time: 2024-01-01T00:00:40Z, host: "a", _value: 13.0},
            ],
        )
            |> group(columns: ["host"])
            |> difference(nonNegative: true, keepFirst: true)
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(3, result->as_table().rows.size());
    EXPECT_TRUE(result->as_table().rows[0]->lookup("_value")->is_null());
    EXPECT_TRUE(result->as_table().rows[1]->lookup("_value")->is_null());
    EXPECT_EQ("6", result->as_table().rows[2]->lookup("_value")->string());
}

TEST(RuntimeEvalTest, EvaluatesDerivativeBuiltin) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_time: 2024-01-01T00:00:00Z, host: "a", _value: 10.0},
                {_time: 2024-01-01T00:00:10Z, host: "b", _value: 20.0},
                {_time: 2024-01-01T00:00:30Z, host: "a", _value: 17.0},
                {_time: 2024-01-01T00:01:00Z, host: "b", _value: 15.0},
            ],
        )
            |> group(columns: ["host"])
            |> derivative(unit: 10s)
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(2, result->as_table().rows.size());
    ASSERT_NE(nullptr, result->as_table().rows[0]);
    ASSERT_NE(nullptr, result->as_table().rows[1]);
    EXPECT_EQ("\"a\"", result->as_table().rows[0]->lookup("host")->string());
    EXPECT_EQ("2.33333333333333", result->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("\"b\"", result->as_table().rows[1]->lookup("host")->string());
    EXPECT_EQ("-1", result->as_table().rows[1]->lookup("_value")->string());
}

TEST(RuntimeEvalTest, EvaluatesDerivativeBuiltinWithNonNegativeAndInitialZero) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_time: 2024-01-01T00:00:00Z, host: "a", _value: 10.0},
                {_time: 2024-01-01T00:00:10Z, host: "a", _value: 6.0},
                {_time: 2024-01-01T00:00:20Z, host: "a", _value: 8.0},
            ],
        )
            |> group(columns: ["host"])
            |> derivative(unit: 10s, nonNegative: true, initialZero: true)
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(2, result->as_table().rows.size());
    EXPECT_EQ("6", result->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("2", result->as_table().rows[1]->lookup("_value")->string());
}

TEST(RuntimeEvalTest, EvaluatesAggregateWindowBuiltin) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& expr = ParseAssignmentInit(R"(
        result = from(
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
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(3, result->as_table().rows.size());
    ASSERT_EQ(2, result->as_table().table_count());
    ASSERT_NE(nullptr, result->as_table().rows[0]);
    EXPECT_EQ("20", result->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("{host: \"a\"}", result->as_table().rows[0]->lookup("_group")->string());
    EXPECT_EQ("2024-01-01T00:00:00Z", result->as_table().rows[0]->lookup("_start")->string());
    EXPECT_EQ("2024-01-01T00:01:00Z", result->as_table().rows[0]->lookup("_stop")->string());
    EXPECT_EQ("2024-01-01T00:01:00Z", result->as_table().rows[0]->lookup("_time")->string());
    ASSERT_NE(nullptr, result->as_table().rows[1]);
    EXPECT_EQ("50", result->as_table().rows[1]->lookup("_value")->string());
    ASSERT_NE(nullptr, result->as_table().rows[2]);
    EXPECT_EQ("90", result->as_table().rows[2]->lookup("_value")->string());
}

TEST(RuntimeEvalTest, EvaluatesAggregateWindowColumnAndAggregateVariants) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& sum_expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_time: "2024-01-01T00:00:10Z", usage: 10.0},
                {_time: "2024-01-01T00:00:40Z", usage: 30.0},
                {_time: "2024-01-01T00:01:05Z", usage: 50.0},
            ],
        )
            |> aggregateWindow(every: 1m, fn: sum, column: "usage")
    )");
    auto sum_result = ExpressionEvaluator::Evaluate(sum_expr, env);
    ASSERT_TRUE(sum_result.ok()) << sum_result.status();
    ASSERT_EQ(Value::Type::Table, sum_result->type());
    ASSERT_EQ(2, sum_result->as_table().rows.size());
    EXPECT_EQ("40", sum_result->as_table().rows[0]->lookup("usage")->string());
    EXPECT_EQ("50", sum_result->as_table().rows[1]->lookup("usage")->string());

    const auto& count_expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_time: "2024-01-01T00:00:10Z", _value: 10.0},
                {_time: "2024-01-01T00:00:40Z", _value: 30.0},
                {_time: "2024-01-01T00:01:05Z", _value: 50.0},
            ],
        )
            |> aggregateWindow(every: 1m, fn: count)
    )");
    auto count_result = ExpressionEvaluator::Evaluate(count_expr, env);
    ASSERT_TRUE(count_result.ok()) << count_result.status();
    ASSERT_EQ(Value::Type::Table, count_result->type());
    ASSERT_EQ(2, count_result->as_table().rows.size());
    EXPECT_EQ("2", count_result->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("1", count_result->as_table().rows[1]->lookup("_value")->string());
}

TEST(RuntimeEvalTest, EvaluatesAggregateWindowOffset) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_time: "2024-01-01T00:00:10Z", _value: 10.0},
                {_time: "2024-01-01T00:00:40Z", _value: 30.0},
                {_time: "2024-01-01T00:01:10Z", _value: 50.0},
            ],
        )
            |> aggregateWindow(every: 1m, offset: 30s, fn: mean)
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(2, result->as_table().rows.size());
    EXPECT_EQ("10", result->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("2023-12-31T23:59:30Z", result->as_table().rows[0]->lookup("_start")->string());
    EXPECT_EQ("2024-01-01T00:00:30Z", result->as_table().rows[0]->lookup("_stop")->string());
    EXPECT_EQ("2024-01-01T00:00:30Z", result->as_table().rows[0]->lookup("_time")->string());
    EXPECT_EQ("40", result->as_table().rows[1]->lookup("_value")->string());
    EXPECT_EQ("2024-01-01T00:00:30Z", result->as_table().rows[1]->lookup("_start")->string());
    EXPECT_EQ("2024-01-01T00:01:30Z", result->as_table().rows[1]->lookup("_stop")->string());
}

TEST(RuntimeEvalTest, EvaluatesAggregateWindowCalendarMonths) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_time: "2024-01-15T08:00:00Z", _value: 10.0},
                {_time: "2024-01-20T09:30:00Z", _value: 30.0},
                {_time: "2024-02-02T00:15:00Z", _value: 50.0},
            ],
        )
            |> aggregateWindow(every: 1mo, fn: mean)
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(2, result->as_table().rows.size());
    EXPECT_EQ("20", result->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("2024-01-01T00:00:00Z", result->as_table().rows[0]->lookup("_start")->string());
    EXPECT_EQ("2024-02-01T00:00:00Z", result->as_table().rows[0]->lookup("_stop")->string());
    EXPECT_EQ("50", result->as_table().rows[1]->lookup("_value")->string());
    EXPECT_EQ("2024-02-01T00:00:00Z", result->as_table().rows[1]->lookup("_start")->string());
    EXPECT_EQ("2024-03-01T00:00:00Z", result->as_table().rows[1]->lookup("_stop")->string());
}

TEST(RuntimeEvalTest, EvaluatesAggregateWindowWithTimezoneLocation) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_time: "2024-01-01T07:30:00Z", _value: 10.0},
                {_time: "2024-01-01T08:30:00Z", _value: 30.0},
            ],
        )
            |> aggregateWindow(
                every: 1d,
                fn: sum,
                location: {zone: "UTC", offset: "-8h"},
            )
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(2, result->as_table().rows.size());
    EXPECT_EQ("10", result->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("2023-12-31T08:00:00Z", result->as_table().rows[0]->lookup("_start")->string());
    EXPECT_EQ("2024-01-01T08:00:00Z", result->as_table().rows[0]->lookup("_stop")->string());
    EXPECT_EQ("30", result->as_table().rows[1]->lookup("_value")->string());
    EXPECT_EQ("2024-01-01T08:00:00Z", result->as_table().rows[1]->lookup("_start")->string());
    EXPECT_EQ("2024-01-02T08:00:00Z", result->as_table().rows[1]->lookup("_stop")->string());
}

TEST(RuntimeEvalTest, EvaluatesAggregateWindowWithGlobalOptionLocation) {
    Environment env;
    BuiltinRegistry::Install(env);
    env.define_option("location",
                      Value::object({
                          {"zone", Value::string("UTC")},
                          {"offset", Value::duration("-8h")},
                      }));

    const auto& expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_time: "2024-01-01T07:30:00Z", _value: 10.0},
                {_time: "2024-01-01T08:30:00Z", _value: 30.0},
            ],
        )
            |> aggregateWindow(every: 1d, fn: sum)
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(2, result->as_table().rows.size());
    EXPECT_EQ("10", result->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("2023-12-31T08:00:00Z", result->as_table().rows[0]->lookup("_start")->string());
    EXPECT_EQ("2024-01-01T08:00:00Z", result->as_table().rows[0]->lookup("_stop")->string());
    EXPECT_EQ("30", result->as_table().rows[1]->lookup("_value")->string());
    EXPECT_EQ("2024-01-01T08:00:00Z", result->as_table().rows[1]->lookup("_start")->string());
    EXPECT_EQ("2024-01-02T08:00:00Z", result->as_table().rows[1]->lookup("_stop")->string());
}

TEST(RuntimeEvalTest, AggregateWindowExplicitLocationOverridesGlobalOptionLocation) {
    Environment env;
    BuiltinRegistry::Install(env);
    env.define_option("location",
                      Value::object({
                          {"zone", Value::string("UTC")},
                          {"offset", Value::duration("-8h")},
                      }));

    const auto& expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_time: "2024-01-01T07:30:00Z", _value: 10.0},
                {_time: "2024-01-01T08:30:00Z", _value: 30.0},
            ],
        )
            |> aggregateWindow(
                every: 1d,
                fn: sum,
                location: {zone: "UTC", offset: 0s},
            )
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(1, result->as_table().rows.size());
    EXPECT_EQ("40", result->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("2024-01-01T00:00:00Z", result->as_table().rows[0]->lookup("_start")->string());
    EXPECT_EQ("2024-01-02T00:00:00Z", result->as_table().rows[0]->lookup("_stop")->string());
}

TEST(RuntimeEvalTest, EvaluatesAggregateWindowTimezoneOutputShape) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& expr = ParseAssignmentInit(R"(
        result = from(
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
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(1, result->as_table().rows.size());
    const auto& row = result->as_table().rows[0];
    ASSERT_NE(nullptr, row);
    EXPECT_EQ("10", row->lookup("_value")->string());
    EXPECT_EQ("\"edge-1\"", row->lookup("host")->string());
    EXPECT_EQ("2024-03-01T08:00:00Z", row->lookup("_start")->string());
    EXPECT_EQ("2024-04-01T07:00:00Z", row->lookup("_stop")->string());
    EXPECT_EQ("2024-03-01T08:00:00Z", row->lookup("bucket_time")->string());
    EXPECT_EQ(nullptr, row->lookup("_time"));
    EXPECT_EQ(nullptr, row->lookup("region"));
    EXPECT_EQ(nullptr, row->lookup("note"));
}

TEST(RuntimeEvalTest, EvaluatesAggregateWindowWithPeriodDifferentFromEvery) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_time: "2024-01-01T00:00:00Z", _value: 2.0},
                {_time: "2024-01-01T00:00:10Z", _value: 4.0},
                {_time: "2024-01-01T00:00:20Z", _value: 6.0},
                {_time: "2024-01-01T00:00:30Z", _value: 8.0},
                {_time: "2024-01-01T00:00:40Z", _value: 10.0},
                {_time: "2024-01-01T00:00:50Z", _value: 12.0},
            ],
        )
            |> range(start: 2024-01-01T00:00:00Z, stop: 2024-01-01T00:01:00Z)
            |> aggregateWindow(every: 20s, period: 40s, fn: count, createEmpty: false)
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(2, result->as_table().rows.size());
    EXPECT_EQ("4", result->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("2024-01-01T00:00:00Z", result->as_table().rows[0]->lookup("_start")->string());
    EXPECT_EQ("2024-01-01T00:00:40Z", result->as_table().rows[0]->lookup("_stop")->string());
    EXPECT_EQ("4", result->as_table().rows[1]->lookup("_value")->string());
    EXPECT_EQ("2024-01-01T00:00:20Z", result->as_table().rows[1]->lookup("_start")->string());
    EXPECT_EQ("2024-01-01T00:01:00Z", result->as_table().rows[1]->lookup("_stop")->string());
}

TEST(RuntimeEvalTest, AggregateWindowPreservesDistinctChunksWithSameGroupKey) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& expr = ParseAssignmentInit(R"(
        result = union(
            tables: [
                from(
                    bucket: "telegraf",
                    rows: [
                        {_time: "2024-01-01T00:00:10Z", host: "a", _value: 10.0},
                        {_time: "2024-01-01T00:00:20Z", host: "a", _value: 30.0},
                    ],
                ) |> group(columns: ["host"]),
                from(
                    bucket: "telegraf",
                    rows: [
                        {_time: "2024-01-01T00:00:10Z", host: "a", _value: 100.0},
                        {_time: "2024-01-01T00:00:20Z", host: "a", _value: 300.0},
                    ],
                ) |> group(columns: ["host"]),
            ],
        )
            |> aggregateWindow(every: 1m, fn: mean)
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(2, result->as_table().table_count());
    ASSERT_EQ(2, result->as_table().rows.size());
    ASSERT_EQ(1, result->as_table().tables[0].rows.size());
    ASSERT_EQ(1, result->as_table().tables[1].rows.size());
    EXPECT_EQ("20", result->as_table().tables[0].rows[0]->lookup("_value")->string());
    EXPECT_EQ("200", result->as_table().tables[1].rows[0]->lookup("_value")->string());
    EXPECT_EQ("{host: \"a\"}", result->as_table().tables[0].rows[0]->lookup("_group")->string());
    EXPECT_EQ("{host: \"a\"}", result->as_table().tables[1].rows[0]->lookup("_group")->string());
}

TEST(RuntimeEvalTest, EvaluatesAggregateWindowWithNegativePeriod) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_time: "2024-01-01T00:00:00Z", _value: 2.0},
                {_time: "2024-01-01T00:00:10Z", _value: 4.0},
                {_time: "2024-01-01T00:00:20Z", _value: 6.0},
                {_time: "2024-01-01T00:00:30Z", _value: 8.0},
                {_time: "2024-01-01T00:00:40Z", _value: 10.0},
                {_time: "2024-01-01T00:00:50Z", _value: 12.0},
            ],
        )
            |> range(start: 2024-01-01T00:00:00Z, stop: 2024-01-01T00:01:00Z)
            |> aggregateWindow(every: 20s, period: "-40s", fn: count, createEmpty: false)
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(2, result->as_table().rows.size());
    EXPECT_EQ("4", result->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("2024-01-01T00:00:40Z", result->as_table().rows[0]->lookup("_start")->string());
    EXPECT_EQ("2024-01-01T00:00:00Z", result->as_table().rows[0]->lookup("_stop")->string());
    EXPECT_EQ("4", result->as_table().rows[1]->lookup("_value")->string());
    EXPECT_EQ("2024-01-01T00:01:00Z", result->as_table().rows[1]->lookup("_start")->string());
    EXPECT_EQ("2024-01-01T00:00:20Z", result->as_table().rows[1]->lookup("_stop")->string());
}

TEST(RuntimeEvalTest, EvaluatesAggregateWindowCalendarOffset) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_time: "2024-01-20T00:00:00Z", _value: 10.0},
                {_time: "2024-02-20T00:00:00Z", _value: 30.0},
            ],
        )
            |> aggregateWindow(every: 1mo, offset: 15d, fn: mean, createEmpty: false)
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(2, result->as_table().rows.size());
    EXPECT_EQ("10", result->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("2024-01-16T00:00:00Z", result->as_table().rows[0]->lookup("_start")->string());
    EXPECT_EQ("2024-02-16T00:00:00Z", result->as_table().rows[0]->lookup("_stop")->string());
    EXPECT_EQ("30", result->as_table().rows[1]->lookup("_value")->string());
    EXPECT_EQ("2024-02-16T00:00:00Z", result->as_table().rows[1]->lookup("_start")->string());
    EXPECT_EQ("2024-03-16T00:00:00Z", result->as_table().rows[1]->lookup("_stop")->string());
}

TEST(RuntimeEvalTest, EvaluatesAggregateWindowCreateEmptyAcrossRange) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_time: "2024-01-01T00:00:10Z", _value: 10.0, host: "a"},
            ],
        )
            |> range(start: 2024-01-01T00:00:00Z, stop: 2024-01-01T00:03:00Z)
            |> group(columns: ["host"])
            |> aggregateWindow(every: 1m, fn: count)
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(3, result->as_table().rows.size());
    EXPECT_EQ("1", result->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("0", result->as_table().rows[1]->lookup("_value")->string());
    EXPECT_EQ("0", result->as_table().rows[2]->lookup("_value")->string());
    EXPECT_EQ("2024-01-01T00:00:00Z", result->as_table().rows[0]->lookup("_start")->string());
    EXPECT_EQ("2024-01-01T00:01:00Z", result->as_table().rows[1]->lookup("_start")->string());
    EXPECT_EQ("2024-01-01T00:02:00Z", result->as_table().rows[2]->lookup("_start")->string());
}

TEST(RuntimeEvalTest, EvaluatesAggregateWindowSelectorDropsEmptyWindows) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& expr = ParseAssignmentInit(R"(
        result = from(
            bucket: "telegraf",
            rows: [
                {_time: "2024-01-01T00:00:10Z", _value: 10.0},
                {_time: "2024-01-01T00:02:05Z", _value: 30.0},
            ],
        )
            |> range(start: 2024-01-01T00:00:00Z, stop: 2024-01-01T00:03:00Z)
            |> aggregateWindow(every: 1m, fn: last, createEmpty: true)
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(2, result->as_table().rows.size());
    EXPECT_EQ("10", result->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("2024-01-01T00:00:00Z", result->as_table().rows[0]->lookup("_start")->string());
    EXPECT_EQ("30", result->as_table().rows[1]->lookup("_value")->string());
    EXPECT_EQ("2024-01-01T00:02:00Z", result->as_table().rows[1]->lookup("_start")->string());
}

TEST(RuntimeEvalTest, ReportsAggregateWindowArgumentErrors) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& invalid_every_expr = ParseAssignmentInit(R"(
        result = from(bucket: "telegraf", rows: [{_time: "2024-01-01T00:00:10Z", _value: 1.0}])
            |> aggregateWindow(every: 0m, fn: mean)
    )");
    auto invalid_every = ExpressionEvaluator::Evaluate(invalid_every_expr, env);
    ASSERT_FALSE(invalid_every.ok());
    EXPECT_EQ(absl::StatusCode::kInvalidArgument, invalid_every.status().code());

    const auto& invalid_offset_expr = ParseAssignmentInit(R"(
        result = from(bucket: "telegraf", rows: [{_time: "2024-01-01T00:00:10Z", _value: 1.0}])
            |> aggregateWindow(every: 1m, offset: "bad", fn: mean)
    )");
    auto invalid_offset = ExpressionEvaluator::Evaluate(invalid_offset_expr, env);
    ASSERT_FALSE(invalid_offset.ok());
    EXPECT_EQ(absl::StatusCode::kInvalidArgument, invalid_offset.status().code());

    const auto& missing_fn_expr = ParseAssignmentInit(R"(
        result = from(bucket: "telegraf", rows: [{_time: "2024-01-01T00:00:10Z", _value: 1.0}])
            |> aggregateWindow(every: 1m)
    )");
    auto missing_fn = ExpressionEvaluator::Evaluate(missing_fn_expr, env);
    ASSERT_FALSE(missing_fn.ok());
    EXPECT_EQ(absl::StatusCode::kInvalidArgument, missing_fn.status().code());

    const auto& create_empty_expr = ParseAssignmentInit(R"(
        result = from(bucket: "telegraf", rows: [
                {_time: "2024-01-01T00:00:10Z", _value: 10.0, host: "a"},
                {_time: "2024-01-01T00:02:05Z", _value: 30.0, host: "a"},
            ])
            |> group(columns: ["host"])
            |> aggregateWindow(every: 1m, fn: mean, createEmpty: true)
    )");
    auto create_empty = ExpressionEvaluator::Evaluate(create_empty_expr, env);
    ASSERT_TRUE(create_empty.ok()) << create_empty.status();
    ASSERT_EQ(Value::Type::Table, create_empty->type());
    ASSERT_EQ(3, create_empty->as_table().rows.size());
    ASSERT_NE(nullptr, create_empty->as_table().rows[0]);
    EXPECT_EQ("10", create_empty->as_table().rows[0]->lookup("_value")->string());
    ASSERT_NE(nullptr, create_empty->as_table().rows[1]);
    EXPECT_TRUE(create_empty->as_table().rows[1]->lookup("_value")->is_null());
    EXPECT_EQ("2024-01-01T00:02:00Z", create_empty->as_table().rows[1]->lookup("_time")->string());
    ASSERT_NE(nullptr, create_empty->as_table().rows[2]);
    EXPECT_EQ("30", create_empty->as_table().rows[2]->lookup("_value")->string());

    const auto& create_empty_count_expr = ParseAssignmentInit(R"(
        result = from(bucket: "telegraf", rows: [
                {_time: "2024-01-01T00:00:10Z", _value: 10.0},
                {_time: "2024-01-01T00:02:05Z", _value: 30.0},
            ])
            |> aggregateWindow(every: 1m, fn: count, createEmpty: true)
    )");
    auto create_empty_count = ExpressionEvaluator::Evaluate(create_empty_count_expr, env);
    ASSERT_TRUE(create_empty_count.ok()) << create_empty_count.status();
    ASSERT_EQ(3, create_empty_count->as_table().rows.size());
    EXPECT_EQ("1", create_empty_count->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("0", create_empty_count->as_table().rows[1]->lookup("_value")->string());
    EXPECT_EQ("1", create_empty_count->as_table().rows[2]->lookup("_value")->string());

    const auto& invalid_fixed_period_expr = ParseAssignmentInit(R"(
        result = from(bucket: "telegraf", rows: [{_time: "2024-01-01T00:00:10Z", _value: 1.0}])
            |> aggregateWindow(every: 1m, period: 1mo, fn: mean)
    )");
    auto invalid_fixed_period = ExpressionEvaluator::Evaluate(invalid_fixed_period_expr, env);
    ASSERT_FALSE(invalid_fixed_period.ok());
    EXPECT_EQ(absl::StatusCode::kInvalidArgument, invalid_fixed_period.status().code());
}

TEST(RuntimeEvalTest, EvaluatesWindowBuiltinWithCreateEmpty) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& expr = ParseAssignmentInit(R"(
        result = from(bucket: "telegraf", rows: [
                {_time: "2024-01-01T00:00:10Z", _value: 10.0, host: "a"},
                {_time: "2024-01-01T00:02:05Z", _value: 30.0, host: "a"},
            ])
            |> range(start: 2024-01-01T00:00:00Z, stop: 2024-01-01T00:03:00Z)
            |> group(columns: ["host"])
            |> window(every: 1m, createEmpty: true)
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(3, result->as_table().table_count());
    ASSERT_EQ(2, result->as_table().rows.size());
    ASSERT_NE(nullptr, result->as_table().rows[0]);
    EXPECT_EQ("2024-01-01T00:00:00Z", result->as_table().rows[0]->lookup("_start")->string());
    EXPECT_EQ("2024-01-01T00:01:00Z", result->as_table().rows[0]->lookup("_stop")->string());
    ASSERT_EQ(0, result->as_table().tables[1].rows.size());
    ASSERT_NE(nullptr, result->as_table().tables[1].group_key);
    EXPECT_EQ("2024-01-01T00:01:00Z",
              result->as_table().tables[1].group_key->lookup("_start")->string());
    ASSERT_NE(nullptr, result->as_table().rows[1]);
    EXPECT_EQ("2024-01-01T00:02:00Z", result->as_table().rows[1]->lookup("_start")->string());
}

TEST(RuntimeEvalTest, EvaluatesOuterJoinMethods) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& expr = ParseAssignmentInit(R"(
        result = join(
            tables: {
                cpu: from(bucket: "cpu", rows: [
                    {_time: "t1", host: "a", _value: 90.0},
                    {_time: "t2", host: "a", _value: 91.0},
                ]),
                mem: from(bucket: "mem", rows: [
                    {_time: "t2", host: "a", _value: 40.0},
                    {_time: "t3", host: "a", _value: 20.0},
                ]),
            },
            method: "full",
            on: ["_time"],
        )
    )");
    auto result = ExpressionEvaluator::Evaluate(expr, env);

    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(Value::Type::Table, result->type());
    ASSERT_EQ(3, result->as_table().rows.size());
    ASSERT_NE(nullptr, result->as_table().rows[0]);
    EXPECT_EQ("\"t1\"", result->as_table().rows[0]->lookup("_time")->string());
    EXPECT_EQ("90", result->as_table().rows[0]->lookup("_value_cpu")->string());
    EXPECT_TRUE(result->as_table().rows[0]->lookup("_value_mem")->is_null());
    ASSERT_NE(nullptr, result->as_table().rows[1]);
    EXPECT_EQ("\"t2\"", result->as_table().rows[1]->lookup("_time")->string());
    EXPECT_EQ("91", result->as_table().rows[1]->lookup("_value_cpu")->string());
    EXPECT_EQ("40", result->as_table().rows[1]->lookup("_value_mem")->string());
    ASSERT_NE(nullptr, result->as_table().rows[2]);
    EXPECT_EQ("\"t3\"", result->as_table().rows[2]->lookup("_time")->string());
    EXPECT_TRUE(result->as_table().rows[2]->lookup("_value_cpu")->is_null());
    EXPECT_EQ("20", result->as_table().rows[2]->lookup("_value_mem")->string());
}

TEST(RuntimeEvalTest, EvaluatesSpreadQuantileMedianTopAndBottom) {
    Environment env;
    BuiltinRegistry::Install(env);

    const auto& spread_expr = ParseAssignmentInit(R"(
        result = from(bucket: "cpu", rows: [
                {_time: "t1", host: "a", _value: 10.0},
                {_time: "t2", host: "a", _value: 25.0},
                {_time: "t3", host: "b", _value: 40.0},
                {_time: "t4", host: "b", _value: 50.0},
            ])
            |> group(columns: ["host"])
            |> spread()
    )");
    auto spread = ExpressionEvaluator::Evaluate(spread_expr, env);
    ASSERT_TRUE(spread.ok()) << spread.status();
    ASSERT_EQ(2, spread->as_table().rows.size());
    EXPECT_EQ("15", spread->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("10", spread->as_table().rows[1]->lookup("_value")->string());

    const auto& quantile_expr = ParseAssignmentInit(R"(
        result = from(bucket: "cpu", rows: [
                {_time: "t1", host: "a", _value: 10.0},
                {_time: "t2", host: "a", _value: 20.0},
                {_time: "t3", host: "a", _value: 30.0},
                {_time: "t4", host: "a", _value: 40.0},
            ])
            |> quantile(q: 0.25)
    )");
    auto quantile = ExpressionEvaluator::Evaluate(quantile_expr, env);
    ASSERT_TRUE(quantile.ok()) << quantile.status();
    ASSERT_EQ(1, quantile->as_table().rows.size());
    EXPECT_EQ("17.5", quantile->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("0.25", quantile->as_table().rows[0]->lookup("quantile")->string());

    const auto& multi_quantile_expr = ParseAssignmentInit(R"(
        result = from(bucket: "cpu", rows: [
                {_time: "t1", host: "a", _value: 10.0},
                {_time: "t2", host: "a", _value: 20.0},
                {_time: "t3", host: "a", _value: 30.0},
                {_time: "t4", host: "a", _value: 40.0},
            ])
            |> quantile(q: [0.5, 0.75, 0.99, 0.999])
    )");
    auto multi_quantile = ExpressionEvaluator::Evaluate(multi_quantile_expr, env);
    ASSERT_TRUE(multi_quantile.ok()) << multi_quantile.status();
    ASSERT_EQ(4, multi_quantile->as_table().rows.size());
    EXPECT_EQ("25", multi_quantile->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("0.5", multi_quantile->as_table().rows[0]->lookup("quantile")->string());
    EXPECT_EQ("32.5", multi_quantile->as_table().rows[1]->lookup("_value")->string());
    EXPECT_EQ("0.75", multi_quantile->as_table().rows[1]->lookup("quantile")->string());
    EXPECT_EQ("39.7", multi_quantile->as_table().rows[2]->lookup("_value")->string());
    EXPECT_EQ("0.99", multi_quantile->as_table().rows[2]->lookup("quantile")->string());
    EXPECT_EQ("39.97", multi_quantile->as_table().rows[3]->lookup("_value")->string());
    EXPECT_EQ("0.999", multi_quantile->as_table().rows[3]->lookup("quantile")->string());

    const auto& median_expr = ParseAssignmentInit(R"(
        result = from(bucket: "cpu", rows: [
                {_time: "t1", host: "a", _value: 10.0},
                {_time: "t2", host: "a", _value: 20.0},
                {_time: "t3", host: "a", _value: 30.0},
                {_time: "t4", host: "a", _value: 40.0},
            ])
            |> median()
    )");
    auto median = ExpressionEvaluator::Evaluate(median_expr, env);
    ASSERT_TRUE(median.ok()) << median.status();
    ASSERT_EQ(1, median->as_table().rows.size());
    EXPECT_EQ("25", median->as_table().rows[0]->lookup("_value")->string());

    const auto& top_expr = ParseAssignmentInit(R"(
        result = from(bucket: "cpu", rows: [
                {_time: "t1", host: "a", _value: 10.0},
                {_time: "t2", host: "a", _value: 20.0},
                {_time: "t3", host: "a", _value: 30.0},
            ])
            |> top(n: 2)
    )");
    auto top = ExpressionEvaluator::Evaluate(top_expr, env);
    ASSERT_TRUE(top.ok()) << top.status();
    ASSERT_EQ(2, top->as_table().rows.size());
    EXPECT_EQ("30", top->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("20", top->as_table().rows[1]->lookup("_value")->string());

    const auto& bottom_expr = ParseAssignmentInit(R"(
        result = from(bucket: "cpu", rows: [
                {_time: "t1", host: "a", _value: 10.0},
                {_time: "t2", host: "a", _value: 20.0},
                {_time: "t3", host: "a", _value: 30.0},
            ])
            |> bottom(n: 2)
    )");
    auto bottom = ExpressionEvaluator::Evaluate(bottom_expr, env);
    ASSERT_TRUE(bottom.ok()) << bottom.status();
    ASSERT_EQ(2, bottom->as_table().rows.size());
    EXPECT_EQ("10", bottom->as_table().rows[0]->lookup("_value")->string());
    EXPECT_EQ("20", bottom->as_table().rows[1]->lookup("_value")->string());
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
