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

#include "cpp/pl/flux/ast_debug.h"
#include "cpp/pl/flux/parser.h"

#include <gtest/gtest.h>

namespace pl {
namespace {

TEST(FluxParserTest, ParsesPackageImportsAndPipeExpression) {
    const std::string source = R"(
        package main
        import "array"
        import math "math"

        from(bucket: "telegraf")
            |> range(start: -1h)
            |> filter(fn: (r) => r.host == "local")
    )";

    Parser parser(source);
    auto file = parser.parse_file("pipe.flux");

    ASSERT_NE(file, nullptr);
    EXPECT_TRUE(parser.errors().empty());
    ASSERT_NE(file->package, nullptr);
    EXPECT_EQ("main", file->package->name->name);
    ASSERT_EQ(2, file->imports.size());
    EXPECT_EQ("array", file->imports[0]->path->value);
    ASSERT_NE(file->imports[1]->alias, nullptr);
    EXPECT_EQ("math", file->imports[1]->alias->name);
    ASSERT_EQ(1, file->body.size());
    EXPECT_EQ(Statement::Type::ExpressionStatement, file->body[0]->type);

    const auto& stmt = std::get<std::unique_ptr<ExprStmt>>(file->body[0]->stmt);
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(Expression::Type::PipeExpr, stmt->expression->type);
    EXPECT_NE(stmt->expression->string().find("|>"), std::string::npos);
  }

TEST(FluxParserTest, ParsesBuiltinFunctionTypeWithConstraints) {
    const std::string source = R"(
        builtin sum : (<-tables: [int], ?n: int) => int where A: Addable
    )";

    Parser parser(source);
    auto file = parser.parse_file("builtin.flux");

    ASSERT_NE(file, nullptr);
    EXPECT_TRUE(parser.errors().empty());
    ASSERT_EQ(1, file->body.size());
    ASSERT_EQ(Statement::Type::BuiltinStatement, file->body[0]->type);

    const auto& builtin = std::get<std::unique_ptr<BuiltinStmt>>(file->body[0]->stmt);
    ASSERT_NE(builtin, nullptr);
    ASSERT_NE(builtin->id, nullptr);
    EXPECT_EQ("sum", builtin->id->name);
    ASSERT_NE(builtin->ty, nullptr);
    ASSERT_NE(builtin->ty->monotype, nullptr);
    EXPECT_EQ(MonoType::Type::Function, builtin->ty->monotype->type);
    ASSERT_EQ(1, builtin->ty->constraints.size());
    EXPECT_EQ("A", builtin->ty->constraints[0]->tvar->name);
    EXPECT_EQ("builtin sum: (<-tables: [int], ?n: int) => int where A: Addable",
              builtin->string());
  }

TEST(FluxParserTest, ParsesTestCaseWithExtendsAndReturnBlock) {
    const std::string source = R"(
        testcase aggregate_window_test extends "base" {
            return 1 + 2
        }
    )";

    Parser parser(source);
    auto file = parser.parse_file("testcase.flux");

    ASSERT_NE(file, nullptr);
    EXPECT_TRUE(parser.errors().empty());
    ASSERT_EQ(1, file->body.size());
    ASSERT_EQ(Statement::Type::TestCaseStatement, file->body[0]->type);

    const auto& testcase = std::get<std::unique_ptr<TestCaseStmt>>(file->body[0]->stmt);
    ASSERT_NE(testcase, nullptr);
    ASSERT_NE(testcase->id, nullptr);
    EXPECT_EQ("aggregate_window_test", testcase->id->name);
    ASSERT_NE(testcase->extends, nullptr);
    EXPECT_EQ("base", testcase->extends->value);
    ASSERT_NE(testcase->block, nullptr);
    ASSERT_EQ(1, testcase->block->body.size());
    EXPECT_EQ(Statement::Type::ReturnStatement, testcase->block->body[0]->type);
  }

TEST(FluxParserTest, ParsesRecordTypeAndConditionalExpression) {
    const std::string source = R"(
        builtin mapper : {name: string, value: int}
        result = if exists record.name then "ok" else "missing"
    )";

    Parser parser(source);
    auto file = parser.parse_file("record.flux");

    ASSERT_NE(file, nullptr);
    EXPECT_TRUE(parser.errors().empty());
    ASSERT_EQ(2, file->body.size());
    ASSERT_EQ(Statement::Type::BuiltinStatement, file->body[0]->type);
    ASSERT_EQ(Statement::Type::VariableAssignment, file->body[1]->type);

    const auto& builtin = std::get<std::unique_ptr<BuiltinStmt>>(file->body[0]->stmt);
    ASSERT_NE(builtin, nullptr);
    ASSERT_NE(builtin->ty, nullptr);
    ASSERT_NE(builtin->ty->monotype, nullptr);
    EXPECT_EQ(MonoType::Type::Record, builtin->ty->monotype->type);

    const auto& assignment = std::get<std::unique_ptr<VariableAssgn>>(file->body[1]->stmt);
    ASSERT_NE(assignment, nullptr);
    ASSERT_NE(assignment->init, nullptr);
    EXPECT_EQ(Expression::Type::ConditionalExpr, assignment->init->type);
    EXPECT_NE(assignment->init->string().find("if"), std::string::npos);
    EXPECT_NE(assignment->init->string().find("exists"), std::string::npos);
  }

TEST(FluxParserTest, ParsesStringInterpolationAndRegexMatch) {
    const std::string source = R"(
        message = "hello ${user}"
        matched = value =~ /cpu.*/
    )";

    Parser parser(source);
    auto file = parser.parse_file("literals.flux");

    ASSERT_NE(file, nullptr);
    EXPECT_TRUE(parser.errors().empty());
    ASSERT_EQ(2, file->body.size());

    const auto& message_assignment = std::get<std::unique_ptr<VariableAssgn>>(file->body[0]->stmt);
    ASSERT_NE(message_assignment, nullptr);
    ASSERT_NE(message_assignment->init, nullptr);
    EXPECT_EQ(Expression::Type::StringExpr, message_assignment->init->type);
    EXPECT_EQ("\"hello ${user}\"", message_assignment->init->string());

    const auto& regex_assignment = std::get<std::unique_ptr<VariableAssgn>>(file->body[1]->stmt);
    ASSERT_NE(regex_assignment, nullptr);
    ASSERT_NE(regex_assignment->init, nullptr);
    EXPECT_EQ(Expression::Type::BinaryExpr, regex_assignment->init->type);
    EXPECT_NE(regex_assignment->init->string().find("=~"), std::string::npos);
    EXPECT_NE(regex_assignment->init->string().find("/cpu.*/"), std::string::npos);
  }

TEST(FluxParserTest, ParsesBooleanRecordUpdateAndIndexExpressions) {
    const std::string source = R"(
        updated = {base with enabled: true, retries: 3}
        enabled = updated["enabled"]
        retries = [1, 2, 3][0]
    )";

    Parser parser(source);
    auto file = parser.parse_file("composite.flux");

    ASSERT_NE(file, nullptr);
    EXPECT_TRUE(parser.errors().empty());
    ASSERT_EQ(3, file->body.size());

    const auto& updated_assignment = std::get<std::unique_ptr<VariableAssgn>>(file->body[0]->stmt);
    ASSERT_NE(updated_assignment, nullptr);
    ASSERT_NE(updated_assignment->init, nullptr);
    EXPECT_EQ(Expression::Type::ObjectExpr, updated_assignment->init->type);
    EXPECT_EQ("{ base with enabled: true, retries: 3 }", updated_assignment->init->string());

    const auto& enabled_assignment = std::get<std::unique_ptr<VariableAssgn>>(file->body[1]->stmt);
    ASSERT_NE(enabled_assignment, nullptr);
    ASSERT_NE(enabled_assignment->init, nullptr);
    EXPECT_EQ(Expression::Type::MemberExpr, enabled_assignment->init->type);
    EXPECT_EQ("updated.enabled", enabled_assignment->init->string());

    const auto& retries_assignment = std::get<std::unique_ptr<VariableAssgn>>(file->body[2]->stmt);
    ASSERT_NE(retries_assignment, nullptr);
    ASSERT_NE(retries_assignment->init, nullptr);
    EXPECT_EQ(Expression::Type::IndexExpr, retries_assignment->init->type);
    EXPECT_EQ("[ 1, 2, 3 ][0]", retries_assignment->init->string());
}

TEST(FluxParserTest, ParsesCallExpressionWithPositionalArguments) {
    const std::string source = R"(
        matched = contains("cpu", "cpu-total")
    )";

    Parser parser(source);
    auto file = parser.parse_file("call_positional.flux");

    ASSERT_NE(file, nullptr);
    ASSERT_TRUE(parser.errors().empty());
    ASSERT_EQ(1, file->body.size());

    const auto& assignment = std::get<std::unique_ptr<VariableAssgn>>(file->body[0]->stmt);
    ASSERT_NE(assignment, nullptr);
    ASSERT_NE(assignment->init, nullptr);
    ASSERT_EQ(Expression::Type::CallExpr, assignment->init->type);

    const auto& call = std::get<std::unique_ptr<CallExpr>>(assignment->init->expr);
    ASSERT_NE(call, nullptr);
    ASSERT_EQ(2, call->arguments.size());
    EXPECT_EQ(Expression::Type::StringLit, call->arguments[0]->type);
    EXPECT_EQ(Expression::Type::StringLit, call->arguments[1]->type);
    EXPECT_EQ("cpu", call->arguments[0]->string());
    EXPECT_EQ("cpu-total", call->arguments[1]->string());
}

TEST(FluxParserTest, ParsesCallExpressionWithCompositePositionalArguments) {
    const std::string source = R"(
        combined = join([1, 2], {name: "cpu"})
    )";

    Parser parser(source);
    auto file = parser.parse_file("call_composite.flux");

    ASSERT_NE(file, nullptr);
    ASSERT_TRUE(parser.errors().empty());
    ASSERT_EQ(1, file->body.size());

    const auto& assignment = std::get<std::unique_ptr<VariableAssgn>>(file->body[0]->stmt);
    ASSERT_NE(assignment, nullptr);
    ASSERT_NE(assignment->init, nullptr);
    ASSERT_EQ(Expression::Type::CallExpr, assignment->init->type);

    const auto& call = std::get<std::unique_ptr<CallExpr>>(assignment->init->expr);
    ASSERT_NE(call, nullptr);
    ASSERT_EQ(2, call->arguments.size());
    EXPECT_EQ(Expression::Type::ArrayExpr, call->arguments[0]->type);
    EXPECT_EQ(Expression::Type::ObjectExpr, call->arguments[1]->type);
}

TEST(FluxParserTest, ParsesCallExpressionWithIdentifierPositionalArguments) {
    const std::string source = R"(
result = contains(bucket, measurement)
)";

    Parser parser(source);
    auto file = parser.parse_file("call_ident_args.flux");

    ASSERT_NE(file, nullptr);
    EXPECT_TRUE(parser.errors().empty()) << ::testing::PrintToString(parser.errors());
    ASSERT_EQ(1, file->body.size());

    const auto& assignment = std::get<std::unique_ptr<VariableAssgn>>(file->body[0]->stmt);
    ASSERT_NE(assignment, nullptr);
    ASSERT_NE(assignment->init, nullptr);
    ASSERT_EQ(Expression::Type::CallExpr, assignment->init->type);

    const auto& call = std::get<std::unique_ptr<CallExpr>>(assignment->init->expr);
    ASSERT_NE(call, nullptr);
    ASSERT_EQ(2, call->arguments.size());
    ASSERT_EQ(Expression::Type::Identifier, call->arguments[0]->type);
    ASSERT_EQ(Expression::Type::Identifier, call->arguments[1]->type);
    EXPECT_EQ("bucket", call->arguments[0]->string());
    EXPECT_EQ("measurement", call->arguments[1]->string());
}

TEST(FluxParserTest, PreservesNamedCallArguments) {
    const std::string source = R"(
result = range(start: -1h, stop: now())
)";

    Parser parser(source);
    auto file = parser.parse_file("call_named_args.flux");

    ASSERT_NE(file, nullptr);
    EXPECT_TRUE(parser.errors().empty()) << ::testing::PrintToString(parser.errors());
    ASSERT_EQ(1, file->body.size());

    const auto& assignment = std::get<std::unique_ptr<VariableAssgn>>(file->body[0]->stmt);
    ASSERT_NE(assignment, nullptr);
    ASSERT_NE(assignment->init, nullptr);
    ASSERT_EQ(Expression::Type::CallExpr, assignment->init->type);

    const auto& call = std::get<std::unique_ptr<CallExpr>>(assignment->init->expr);
    ASSERT_NE(call, nullptr);
    ASSERT_EQ(1, call->arguments.size());
    ASSERT_EQ(Expression::Type::ObjectExpr, call->arguments[0]->type);

    const auto& object = std::get<std::unique_ptr<ObjectExpr>>(call->arguments[0]->expr);
    ASSERT_NE(object, nullptr);
    ASSERT_EQ(2, object->properties.size());
    EXPECT_EQ("start", object->properties[0]->key->string());
    EXPECT_EQ("stop", object->properties[1]->key->string());
}

TEST(FluxParserTest, ParsesCallExpressionWithMemberAndIndexPositionalArguments) {
    const std::string source = R"(
result = contains(bucket.name, values[0])
)";

    Parser parser(source);
    auto file = parser.parse_file("call_member_index_args.flux");

    ASSERT_NE(file, nullptr);
    EXPECT_TRUE(parser.errors().empty()) << ::testing::PrintToString(parser.errors());
    ASSERT_EQ(1, file->body.size());

    const auto& assignment = std::get<std::unique_ptr<VariableAssgn>>(file->body[0]->stmt);
    ASSERT_NE(assignment, nullptr);
    ASSERT_NE(assignment->init, nullptr);
    ASSERT_EQ(Expression::Type::CallExpr, assignment->init->type);

    const auto& call = std::get<std::unique_ptr<CallExpr>>(assignment->init->expr);
    ASSERT_NE(call, nullptr);
    ASSERT_EQ(2, call->arguments.size());
    EXPECT_EQ(Expression::Type::MemberExpr, call->arguments[0]->type);
    EXPECT_EQ(Expression::Type::IndexExpr, call->arguments[1]->type);
}

TEST(FluxParserTest, ParsesCompositeLiteralsWithTrailingCommas) {
    const std::string source = R"(
values = [1, 2,]
lookup = ["cpu": 1, "mem": 2,]
config = {host: "local", port: 8080,}
)";

    Parser parser(source);
    auto file = parser.parse_file("trailing_commas.flux");

    ASSERT_NE(file, nullptr);
    EXPECT_TRUE(parser.errors().empty()) << ::testing::PrintToString(parser.errors());
    ASSERT_EQ(3, file->body.size());

    const auto& values = std::get<std::unique_ptr<VariableAssgn>>(file->body[0]->stmt);
    ASSERT_NE(values, nullptr);
    ASSERT_EQ(Expression::Type::ArrayExpr, values->init->type);
    const auto& array = std::get<std::unique_ptr<ArrayExpr>>(values->init->expr);
    ASSERT_NE(array, nullptr);
    EXPECT_EQ(2, array->elements.size());

    const auto& lookup = std::get<std::unique_ptr<VariableAssgn>>(file->body[1]->stmt);
    ASSERT_NE(lookup, nullptr);
    ASSERT_EQ(Expression::Type::DictExpr, lookup->init->type);
    const auto& dict = std::get<std::unique_ptr<DictExpr>>(lookup->init->expr);
    ASSERT_NE(dict, nullptr);
    EXPECT_EQ(2, dict->elements.size());

    const auto& config = std::get<std::unique_ptr<VariableAssgn>>(file->body[2]->stmt);
    ASSERT_NE(config, nullptr);
    ASSERT_EQ(Expression::Type::ObjectExpr, config->init->type);
    const auto& object = std::get<std::unique_ptr<ObjectExpr>>(config->init->expr);
    ASSERT_NE(object, nullptr);
    EXPECT_EQ(2, object->properties.size());
}

TEST(FluxParserTest, ParsesComplexFluxProgramIntoExpectedAst) {
    const std::string source = R"(
        package metrics
        import "array"
        import regexp "regexp"

        builtin mapper : (<-tables: [int], fn: {name: string, active: bool}) => {name: string, active: bool} where A: Record

        option task = {name: "cpu", every: 1h}
        option task.offset = 5m

        config = {base with enabled: true, threshold: 0.5, tags: ["cpu", "mem"]}
        lookup = ["cpu": 1, "mem": 2]
        selected = from(bucket: "telegraf")
            |> range(start: -1h)
            |> filter(fn: (r) => r.host == "local" and r._measurement =~ /cpu.*/)
        message = "host ${config.enabled}"
        status = if exists config.enabled then "ok" else "missing"
    )";

    Parser parser(source);
    auto file = parser.parse_file("complex.flux");

    ASSERT_NE(file, nullptr);
    EXPECT_TRUE(parser.errors().empty());
    ASSERT_NE(file->package, nullptr);
    EXPECT_EQ("metrics", file->package->name->name);
    ASSERT_EQ(2, file->imports.size());
    EXPECT_EQ("array", file->imports[0]->path->value);
    ASSERT_NE(file->imports[1]->alias, nullptr);
    EXPECT_EQ("regexp", file->imports[1]->alias->name);
    ASSERT_EQ(8, file->body.size());

    ASSERT_EQ(Statement::Type::BuiltinStatement, file->body[0]->type);
    const auto& builtin = std::get<std::unique_ptr<BuiltinStmt>>(file->body[0]->stmt);
    ASSERT_NE(builtin, nullptr);
    ASSERT_NE(builtin->ty, nullptr);
    ASSERT_NE(builtin->ty->monotype, nullptr);
    EXPECT_EQ(MonoType::Type::Function, builtin->ty->monotype->type);
    EXPECT_EQ("builtin mapper: (<-tables: [int], fn: {name: string, active: bool}) => {name: string, active: bool} where A: Record",
              builtin->string());

    ASSERT_EQ(Statement::Type::OptionStatement, file->body[1]->type);
    const auto& task_option = std::get<std::unique_ptr<OptionStmt>>(file->body[1]->stmt);
    ASSERT_NE(task_option, nullptr);
    ASSERT_NE(task_option->assignment, nullptr);
    EXPECT_EQ(Assignment::Type::VariableAssignment, task_option->assignment->type);

    ASSERT_EQ(Statement::Type::OptionStatement, file->body[2]->type);
    const auto& offset_option = std::get<std::unique_ptr<OptionStmt>>(file->body[2]->stmt);
    ASSERT_NE(offset_option, nullptr);
    ASSERT_NE(offset_option->assignment, nullptr);
    EXPECT_EQ(Assignment::Type::MemberAssignment, offset_option->assignment->type);

    ASSERT_EQ(Statement::Type::VariableAssignment, file->body[3]->type);
    const auto& config_assignment = std::get<std::unique_ptr<VariableAssgn>>(file->body[3]->stmt);
    ASSERT_NE(config_assignment, nullptr);
    ASSERT_NE(config_assignment->init, nullptr);
    EXPECT_EQ(Expression::Type::ObjectExpr, config_assignment->init->type);
    EXPECT_EQ("{ base with enabled: true, threshold: 0.5000, tags: [ cpu, mem ] }",
              config_assignment->init->string());

    ASSERT_EQ(Statement::Type::VariableAssignment, file->body[4]->type);
    const auto& lookup_assignment = std::get<std::unique_ptr<VariableAssgn>>(file->body[4]->stmt);
    ASSERT_NE(lookup_assignment, nullptr);
    ASSERT_NE(lookup_assignment->init, nullptr);
    EXPECT_EQ(Expression::Type::DictExpr, lookup_assignment->init->type);
    EXPECT_EQ("[ cpu: 1, mem: 2 ]", lookup_assignment->init->string());

    ASSERT_EQ(Statement::Type::VariableAssignment, file->body[5]->type);
    const auto& selected_assignment = std::get<std::unique_ptr<VariableAssgn>>(file->body[5]->stmt);
    ASSERT_NE(selected_assignment, nullptr);
    ASSERT_NE(selected_assignment->init, nullptr);
    EXPECT_EQ(Expression::Type::PipeExpr, selected_assignment->init->type);
    EXPECT_NE(selected_assignment->init->string().find("|>"), std::string::npos);
    EXPECT_NE(selected_assignment->init->string().find("=~ /cpu.*/"), std::string::npos);
    EXPECT_NE(selected_assignment->init->string().find("and"), std::string::npos);

    ASSERT_EQ(Statement::Type::VariableAssignment, file->body[6]->type);
    const auto& message_assignment = std::get<std::unique_ptr<VariableAssgn>>(file->body[6]->stmt);
    ASSERT_NE(message_assignment, nullptr);
    ASSERT_NE(message_assignment->init, nullptr);
    EXPECT_EQ(Expression::Type::StringExpr, message_assignment->init->type);
    EXPECT_EQ("\"host ${config.enabled}\"", message_assignment->init->string());

    ASSERT_EQ(Statement::Type::VariableAssignment, file->body[7]->type);
    const auto& status_assignment = std::get<std::unique_ptr<VariableAssgn>>(file->body[7]->stmt);
    ASSERT_NE(status_assignment, nullptr);
    ASSERT_NE(status_assignment->init, nullptr);
    EXPECT_EQ(Expression::Type::ConditionalExpr, status_assignment->init->type);
    EXPECT_EQ("if exists config.enabled then ok else missing", status_assignment->init->string());
}

TEST(FluxParserTest, DumpsAstAsReadableTree) {
    const std::string source = R"(
        package metrics
        import "array"

        config = {base with enabled: true, tags: ["cpu", "mem"]}
        status = if exists config.enabled then "ok" else "missing"
    )";

    Parser parser(source);
    auto file = parser.parse_file("dump.flux");

    ASSERT_NE(file, nullptr);
    ASSERT_TRUE(parser.errors().empty());

    const std::string tree = dump_ast(*file);
    EXPECT_NE(tree.find("File name=\"dump.flux\""), std::string::npos);
    EXPECT_NE(tree.find("PackageClause name=metrics"), std::string::npos);
    EXPECT_NE(tree.find("ImportDeclaration path=\"array\""), std::string::npos);
    EXPECT_NE(tree.find("VariableAssignment id=config"), std::string::npos);
    EXPECT_NE(tree.find("ObjectExpr with=base"), std::string::npos);
    EXPECT_NE(tree.find("Property key=enabled"), std::string::npos);
    EXPECT_NE(tree.find("BooleanLit value=true"), std::string::npos);
    EXPECT_NE(tree.find("ArrayExpr"), std::string::npos);
    EXPECT_NE(tree.find("ConditionalExpr"), std::string::npos);
    EXPECT_NE(tree.find("MemberExpr property=enabled"), std::string::npos);
}

TEST(FluxParserTest, DumpsAstAsJsonTree) {
    const std::string source = R"(
        package metrics
        import "array"
        status = if exists config.enabled then "ok" else "missing"
    )";

    Parser parser(source);
    auto file = parser.parse_file("dump.json.flux");

    ASSERT_NE(file, nullptr);
    ASSERT_TRUE(parser.errors().empty());

    const std::string json = dump_ast_json(*file);
    EXPECT_NE(json.find("\"type\":\"File\""), std::string::npos);
    EXPECT_NE(json.find("\"summary\":\"name=dump.json.flux loc="), std::string::npos);
    EXPECT_NE(json.find("\"type\":\"PackageClause\""), std::string::npos);
    EXPECT_NE(json.find("\"summary\":\"name=metrics loc="), std::string::npos);
    EXPECT_NE(json.find("\"type\":\"ImportDeclaration\""), std::string::npos);
    EXPECT_NE(json.find("\"summary\":\"path=\\\"array\\\" loc="), std::string::npos);
    EXPECT_NE(json.find("\"type\":\"ConditionalExpr\""), std::string::npos);
    EXPECT_NE(json.find("\"type\":\"UnaryExpr\""), std::string::npos);
    EXPECT_NE(json.find("\"summary\":\"op=exists\""), std::string::npos);
}

TEST(FluxParserTest, ParsesDynamicVectorAndStreamTypes) {
    const std::string source = R"(
        builtin dyn : dynamic
        builtin vec : vector[int]
        builtin streamer : stream[string]
    )";

    Parser parser(source);
    auto file = parser.parse_file("types.flux");

    ASSERT_NE(file, nullptr);
    ASSERT_TRUE(parser.errors().empty());
    ASSERT_EQ(3, file->body.size());

    const auto& dyn = std::get<std::unique_ptr<BuiltinStmt>>(file->body[0]->stmt);
    const auto& vec = std::get<std::unique_ptr<BuiltinStmt>>(file->body[1]->stmt);
    const auto& stream = std::get<std::unique_ptr<BuiltinStmt>>(file->body[2]->stmt);

    ASSERT_NE(dyn, nullptr);
    ASSERT_NE(vec, nullptr);
    ASSERT_NE(stream, nullptr);
    EXPECT_EQ(MonoType::Type::Dynamic, dyn->ty->monotype->type);
    EXPECT_EQ(MonoType::Type::Vector, vec->ty->monotype->type);
    EXPECT_EQ(MonoType::Type::Stream, stream->ty->monotype->type);
    EXPECT_EQ("builtin dyn: dynamic", dyn->string());
    EXPECT_EQ("builtin vec: vector[int]", vec->string());
    EXPECT_EQ("builtin streamer: stream[string]", stream->string());
}

TEST(FluxParserTest, ParsesTypeVariablesInFunctionTypes) {
    const std::string source = R"(
        builtin identity : (value: A) => A where A: Addable
    )";

    Parser parser(source);
    auto file = parser.parse_file("tvar.flux");

    ASSERT_NE(file, nullptr);
    ASSERT_TRUE(parser.errors().empty()) << ::testing::PrintToString(parser.errors());
    ASSERT_EQ(1, file->body.size());

    const auto& builtin = std::get<std::unique_ptr<BuiltinStmt>>(file->body[0]->stmt);
    ASSERT_NE(builtin, nullptr);
    ASSERT_NE(builtin->ty, nullptr);
    ASSERT_NE(builtin->ty->monotype, nullptr);
    ASSERT_EQ(MonoType::Type::Function, builtin->ty->monotype->type);
    ASSERT_EQ(1, builtin->ty->constraints.size());
    EXPECT_EQ("A", builtin->ty->constraints[0]->tvar->name);

    const auto& function =
        std::get<std::unique_ptr<FunctionType>>(builtin->ty->monotype->value);
    ASSERT_NE(function, nullptr);
    ASSERT_EQ(1, function->parameters.size());
    ASSERT_EQ(ParameterType::Type::Required, function->parameters[0]->type);

    const auto& required =
        std::get<std::shared_ptr<Required>>(function->parameters[0]->value);
    ASSERT_NE(required, nullptr);
    ASSERT_NE(required->monotype, nullptr);
    ASSERT_NE(function->monotype, nullptr);
    EXPECT_EQ(MonoType::Type::Tvar, required->monotype->type);
    EXPECT_EQ(MonoType::Type::Tvar, function->monotype->type);
    EXPECT_EQ("builtin identity: (value: A) => A where A: Addable", builtin->string());
}

TEST(FluxParserTest, ParsesPackageAttributeWithoutBadStatement) {
    const std::string source = R"(
        @edition("2022.1")
        package metrics
    )";

    Parser parser(source);
    auto file = parser.parse_file("attribute.flux");

    ASSERT_NE(file, nullptr);
    ASSERT_TRUE(parser.errors().empty());
    ASSERT_NE(file->package, nullptr);
    EXPECT_EQ("metrics", file->package->name->name);
    EXPECT_TRUE(file->body.empty());
}

TEST(FluxParserTest, ParsesLabelLiteral) {
    const std::string source = ".field";

    Parser parser(source);
    auto file = parser.parse_file("label.flux");

    ASSERT_NE(file, nullptr);
    ASSERT_TRUE(parser.errors().empty());
    ASSERT_EQ(1, file->body.size());
    ASSERT_EQ(Statement::Type::ExpressionStatement, file->body[0]->type);

    const auto& expr_stmt = std::get<std::unique_ptr<ExprStmt>>(file->body[0]->stmt);
    ASSERT_NE(expr_stmt, nullptr);
    ASSERT_NE(expr_stmt->expression, nullptr);
    EXPECT_EQ(Expression::Type::LabelLit, expr_stmt->expression->type);
    EXPECT_EQ(".field", expr_stmt->expression->string());
}

TEST(FluxParserTest, ParsesUnsignedIntegerLiteral) {
    const std::string source = "counter = 42u";

    Parser parser(source);
    auto file = parser.parse_file("uint.flux");

    ASSERT_NE(file, nullptr);
    ASSERT_TRUE(parser.errors().empty());
    ASSERT_EQ(1, file->body.size());
    ASSERT_EQ(Statement::Type::VariableAssignment, file->body[0]->type);

    const auto& assignment = std::get<std::unique_ptr<VariableAssgn>>(file->body[0]->stmt);
    ASSERT_NE(assignment, nullptr);
    ASSERT_NE(assignment->init, nullptr);
    EXPECT_EQ(Expression::Type::UnsignedIntegerLit, assignment->init->type);
    EXPECT_EQ("42u", assignment->init->string());
}

TEST(FluxParserTest, AttachesAttributesToAstNodes) {
    const std::string source = R"(
        @edition("2022.1")
        package metrics
        @feature(flag)
        import "array"
        @trace("enabled")
        result = 42
    )";

    Parser parser(source);
    auto file = parser.parse_file("attrs.flux");

    ASSERT_NE(file, nullptr);
    ASSERT_TRUE(parser.errors().empty());
    ASSERT_NE(file->package, nullptr);
    ASSERT_EQ(1, file->package->attributes.size());
    EXPECT_EQ("@edition(2022.1) package metrics", file->package->string());

    ASSERT_EQ(1, file->imports.size());
    ASSERT_EQ(1, file->imports[0]->attributes.size());
    EXPECT_EQ("@feature(flag) import array", file->imports[0]->string());

    ASSERT_EQ(1, file->body.size());
    ASSERT_EQ(1, file->body[0]->attributes.size());
    EXPECT_EQ("@trace(enabled) VariableAssignment: result = 42", file->body[0]->string());

    const std::string tree = dump_ast(*file);
    EXPECT_NE(tree.find("PackageClause name=metrics attrs=@edition(2022.1)"), std::string::npos);
    EXPECT_NE(tree.find("ImportDeclaration path=\"array\" attrs=@feature(flag)"), std::string::npos);
    EXPECT_NE(tree.find("VariableAssignment id=result attrs=@trace(enabled)"), std::string::npos);
}

TEST(FluxParserTest, ParsesComplexAttributeParameters) {
    const std::string source = R"(
        @trace(contains(bucket.name, values[0]), {enabled: true}, [1, 2])
        result = 42
    )";

    Parser parser(source);
    auto file = parser.parse_file("complex_attrs.flux");

    ASSERT_NE(file, nullptr);
    ASSERT_TRUE(parser.errors().empty()) << ::testing::PrintToString(parser.errors());
    ASSERT_EQ(1, file->body.size());
    ASSERT_EQ(1, file->body[0]->attributes.size());

    const auto& attr = file->body[0]->attributes[0];
    ASSERT_NE(attr, nullptr);
    ASSERT_EQ("trace", attr->name);
    ASSERT_EQ(3, attr->params.size());
    EXPECT_EQ(Expression::Type::CallExpr, attr->params[0]->value->type);
    EXPECT_EQ(Expression::Type::ObjectExpr, attr->params[1]->value->type);
    EXPECT_EQ(Expression::Type::ArrayExpr, attr->params[2]->value->type);
}

TEST(FluxParserTest, AttachesSourceLocationsToTopLevelNodes) {
    const std::string source = R"(package metrics
import "array"
result = if exists config.enabled then "ok" else "missing"
)";

    Parser parser(source);
    auto file = parser.parse_file("loc.flux");

    ASSERT_NE(file, nullptr);
    ASSERT_TRUE(parser.errors().empty());
    ASSERT_TRUE(file->loc.is_valid());
    EXPECT_EQ(1, file->loc.start.line);
    EXPECT_EQ(1, file->loc.start.column);

    ASSERT_NE(file->package, nullptr);
    ASSERT_TRUE(file->package->loc.is_valid());
    EXPECT_EQ(1, file->package->loc.start.line);
    EXPECT_EQ(1, file->package->loc.start.column);

    ASSERT_EQ(1, file->imports.size());
    ASSERT_TRUE(file->imports[0]->loc.is_valid());
    EXPECT_EQ(2, file->imports[0]->loc.start.line);
    EXPECT_EQ(1, file->imports[0]->loc.start.column);

    ASSERT_EQ(1, file->body.size());
    ASSERT_TRUE(file->body[0]->loc.is_valid());
    EXPECT_EQ(3, file->body[0]->loc.start.line);
    EXPECT_EQ(1, file->body[0]->loc.start.column);

    const auto& assignment = std::get<std::unique_ptr<VariableAssgn>>(file->body[0]->stmt);
    ASSERT_NE(assignment, nullptr);
    ASSERT_NE(assignment->init, nullptr);
    ASSERT_TRUE(assignment->init->loc.is_valid());
    EXPECT_EQ(3, assignment->init->loc.start.line);
    EXPECT_EQ(10, assignment->init->loc.start.column);

    const std::string tree = dump_ast(*file);
    EXPECT_NE(tree.find("File name=\"loc.flux\" loc=1:1-3:59"), std::string::npos);
    EXPECT_NE(tree.find("PackageClause name=metrics loc=1:1-1:16"), std::string::npos);
    EXPECT_NE(tree.find("ImportDeclaration path=\"array\" loc=2:1-2:15"), std::string::npos);
    EXPECT_NE(tree.find("VariableAssignment id=result loc=3:1-3:59"), std::string::npos);

    const std::string json = dump_ast_json(*file);
    EXPECT_NE(json.find("\"summary\":\"name=loc.flux loc=1:1-3:59\""), std::string::npos);
    EXPECT_NE(json.find("\"summary\":\"name=metrics loc=1:1-1:16\""), std::string::npos);
    EXPECT_NE(json.find("\"summary\":\"path=\\\"array\\\" loc=2:1-2:15\""), std::string::npos);
}

TEST(FluxParserTest, RecoversFromInvalidObjectProperty) {
    const std::string source = R"(
config = {host: "local", 1, port: 8080}
next = 42
)";

    Parser parser(source);
    auto file = parser.parse_file("invalid_property.flux");

    ASSERT_NE(file, nullptr);
    ASSERT_FALSE(parser.errors().empty());
    ASSERT_EQ(2, file->body.size());
    ASSERT_EQ(Statement::Type::VariableAssignment, file->body[0]->type);
    ASSERT_EQ(Statement::Type::VariableAssignment, file->body[1]->type);

    const auto& config = std::get<std::unique_ptr<VariableAssgn>>(file->body[0]->stmt);
    ASSERT_NE(config, nullptr);
    ASSERT_NE(config->init, nullptr);
    ASSERT_EQ(Expression::Type::ObjectExpr, config->init->type);

    const auto& object = std::get<std::unique_ptr<ObjectExpr>>(config->init->expr);
    ASSERT_NE(object, nullptr);
    ASSERT_EQ(3, object->properties.size());
    EXPECT_EQ("host", object->properties[0]->key->string());
    EXPECT_EQ("<invalid>", object->properties[1]->key->string());
    EXPECT_EQ("port", object->properties[2]->key->string());

    const auto& next = std::get<std::unique_ptr<VariableAssgn>>(file->body[1]->stmt);
    ASSERT_NE(next, nullptr);
    EXPECT_EQ("next", next->id->name);
    EXPECT_EQ("42", next->init->string());
}

TEST(FluxParserTest, RecoversFromMissingPropertyValue) {
    const std::string source = R"(
config = {host:, port: 8080}
next = 42
)";

    Parser parser(source);
    auto file = parser.parse_file("missing_property_value.flux");

    ASSERT_NE(file, nullptr);
    ASSERT_FALSE(parser.errors().empty());
    ASSERT_EQ(2, file->body.size());

    const auto& config = std::get<std::unique_ptr<VariableAssgn>>(file->body[0]->stmt);
    ASSERT_NE(config, nullptr);
    ASSERT_NE(config->init, nullptr);
    ASSERT_EQ(Expression::Type::ObjectExpr, config->init->type);

    const auto& object = std::get<std::unique_ptr<ObjectExpr>>(config->init->expr);
    ASSERT_NE(object, nullptr);
    ASSERT_EQ(2, object->properties.size());
    EXPECT_EQ("host", object->properties[0]->key->string());
    EXPECT_EQ(nullptr, object->properties[0]->value);
    EXPECT_EQ("port", object->properties[1]->key->string());
    ASSERT_NE(object->properties[1]->value, nullptr);
    EXPECT_EQ("8080", object->properties[1]->value->string());

    const auto& next = std::get<std::unique_ptr<VariableAssgn>>(file->body[1]->stmt);
    ASSERT_NE(next, nullptr);
    EXPECT_EQ("next", next->id->name);
    EXPECT_EQ("42", next->init->string());
}

TEST(FluxParserTest, RecoversFromInvalidAttributeParameterAndContinues) {
    const std::string source = R"(
        @trace(, "ok")
        result = 42
    )";

    Parser parser(source);
    auto file = parser.parse_file("invalid_attr_param.flux");

    ASSERT_NE(file, nullptr);
    ASSERT_FALSE(parser.errors().empty());
    ASSERT_EQ(1, file->body.size());
    ASSERT_EQ(1, file->body[0]->attributes.size());

    const auto& attr = file->body[0]->attributes[0];
    ASSERT_NE(attr, nullptr);
    ASSERT_EQ(1, attr->params.size());
    EXPECT_EQ(Expression::Type::StringLit, attr->params[0]->value->type);

    const auto& result = std::get<std::unique_ptr<VariableAssgn>>(file->body[0]->stmt);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ("result", result->id->name);
    EXPECT_EQ("42", result->init->string());
}

TEST(FluxParserTest, ReportsInvalidVectorTypeAndContinuesToNextStatement) {
    const std::string source = R"(
        builtin vec : vector[
        next = 42
    )";

    Parser parser(source);
    auto file = parser.parse_file("invalid_vector_type.flux");

    ASSERT_NE(file, nullptr);
    ASSERT_FALSE(parser.errors().empty());
    ASSERT_GE(file->body.size(), 2);
    ASSERT_EQ(Statement::Type::BuiltinStatement, file->body[0]->type);
    ASSERT_EQ(Statement::Type::VariableAssignment, file->body[1]->type);

    const auto& next = std::get<std::unique_ptr<VariableAssgn>>(file->body[1]->stmt);
    ASSERT_NE(next, nullptr);
    EXPECT_EQ("next", next->id->name);
    EXPECT_EQ("42", next->init->string());
}

TEST(FluxParserTest, RecoversFromMissingThenKeyword) {
    const std::string source = R"(
status = if exists ready "ok" else "bad"
)";

    Parser parser(source);
    auto file = parser.parse_file("missing_then.flux");

    ASSERT_NE(file, nullptr);
    ASSERT_FALSE(parser.errors().empty());
    ASSERT_EQ(1, file->body.size());

    const auto& status = std::get<std::unique_ptr<VariableAssgn>>(file->body[0]->stmt);
    ASSERT_NE(status, nullptr);
    ASSERT_NE(status->init, nullptr);
    EXPECT_EQ(Expression::Type::ConditionalExpr, status->init->type);

    const auto& cond = std::get<std::unique_ptr<ConditionalExpr>>(status->init->expr);
    ASSERT_NE(cond, nullptr);
    ASSERT_NE(cond->test, nullptr);
    ASSERT_NE(cond->alternate, nullptr);
    EXPECT_EQ(Expression::Type::BadExpr, cond->consequent->type);
    EXPECT_EQ(Expression::Type::StringLit, cond->alternate->type);
    EXPECT_EQ("bad", std::get<std::unique_ptr<StringLit>>(cond->alternate->expr)->value);

}

TEST(FluxParserTest, RecoversFromInvalidFunctionParameter) {
    const std::string source = R"(
fn = (r,, limit=5) => r
next = 42
)";

    Parser parser(source);
    auto file = parser.parse_file("invalid_param.flux");

    ASSERT_NE(file, nullptr);
    ASSERT_FALSE(parser.errors().empty());
    ASSERT_EQ(2, file->body.size());

    const auto& fn = std::get<std::unique_ptr<VariableAssgn>>(file->body[0]->stmt);
    ASSERT_NE(fn, nullptr);
    ASSERT_NE(fn->init, nullptr);
    ASSERT_EQ(Expression::Type::FunctionExpr, fn->init->type);

    const auto& func = std::get<std::unique_ptr<FunctionExpr>>(fn->init->expr);
    ASSERT_NE(func, nullptr);
    ASSERT_EQ(3, func->params.size());
    EXPECT_EQ("r", func->params[0]->key->string());
    EXPECT_EQ("<invalid-param>", func->params[1]->key->string());
    EXPECT_EQ("limit", func->params[2]->key->string());
    ASSERT_NE(func->body, nullptr);

    const auto& next = std::get<std::unique_ptr<VariableAssgn>>(file->body[1]->stmt);
    ASSERT_NE(next, nullptr);
    EXPECT_EQ("next", next->id->name);
    EXPECT_EQ("42", next->init->string());
}

TEST(FluxParserTest, RecoversFromInvalidCallArgumentProperty) {
    const std::string source = R"(
result = from(bucket: "telegraf",, start: -1h)
next = 42
)";

    Parser parser(source);
    auto file = parser.parse_file("invalid_call_args.flux");

    ASSERT_NE(file, nullptr);
    ASSERT_FALSE(parser.errors().empty());
    ASSERT_EQ(2, file->body.size());

    const auto& result = std::get<std::unique_ptr<VariableAssgn>>(file->body[0]->stmt);
    ASSERT_NE(result, nullptr);
    ASSERT_NE(result->init, nullptr);
    ASSERT_EQ(Expression::Type::CallExpr, result->init->type);

    const auto& call = std::get<std::unique_ptr<CallExpr>>(result->init->expr);
    ASSERT_NE(call, nullptr);
    ASSERT_EQ(1, call->arguments.size());
    ASSERT_EQ(Expression::Type::ObjectExpr, call->arguments[0]->type);

    const auto& object = std::get<std::unique_ptr<ObjectExpr>>(call->arguments[0]->expr);
    ASSERT_NE(object, nullptr);
    ASSERT_EQ(3, object->properties.size());
    EXPECT_EQ("bucket", object->properties[0]->key->string());
    EXPECT_EQ("<invalid>", object->properties[1]->key->string());
    EXPECT_EQ("start", object->properties[2]->key->string());

    const auto& next = std::get<std::unique_ptr<VariableAssgn>>(file->body[1]->stmt);
    ASSERT_NE(next, nullptr);
    EXPECT_EQ("next", next->id->name);
    EXPECT_EQ("42", next->init->string());
}

TEST(FluxParserTest, RecoversFromInvalidArrayElement) {
    const std::string source = R"(
values = [1,,3]
next = 42
)";

    Parser parser(source);
    auto file = parser.parse_file("invalid_array.flux");

    ASSERT_NE(file, nullptr);
    ASSERT_FALSE(parser.errors().empty());
    ASSERT_EQ(2, file->body.size());

    const auto& values = std::get<std::unique_ptr<VariableAssgn>>(file->body[0]->stmt);
    ASSERT_NE(values, nullptr);
    ASSERT_NE(values->init, nullptr);
    ASSERT_EQ(Expression::Type::ArrayExpr, values->init->type);

    const auto& array = std::get<std::unique_ptr<ArrayExpr>>(values->init->expr);
    ASSERT_NE(array, nullptr);
    ASSERT_EQ(2, array->elements.size());
    EXPECT_EQ("1", array->elements[0]->expression->string());
    EXPECT_EQ("3", array->elements[1]->expression->string());

    const auto& next = std::get<std::unique_ptr<VariableAssgn>>(file->body[1]->stmt);
    ASSERT_NE(next, nullptr);
    EXPECT_EQ("next", next->id->name);
    EXPECT_EQ("42", next->init->string());
}

TEST(FluxParserTest, RecoversFromLeadingEmptyObjectProperty) {
    const std::string source = R"(
config = {, host: "local", port: 8080}
next = 42
)";

    Parser parser(source);
    auto file = parser.parse_file("leading_empty_object.flux");

    ASSERT_NE(file, nullptr);
    ASSERT_FALSE(parser.errors().empty());
    ASSERT_EQ(2, file->body.size());

    const auto& config = std::get<std::unique_ptr<VariableAssgn>>(file->body[0]->stmt);
    ASSERT_NE(config, nullptr);
    ASSERT_NE(config->init, nullptr);
    ASSERT_EQ(Expression::Type::ObjectExpr, config->init->type);

    const auto& object = std::get<std::unique_ptr<ObjectExpr>>(config->init->expr);
    ASSERT_NE(object, nullptr);
    ASSERT_EQ(3, object->properties.size());
    EXPECT_EQ("<invalid>", object->properties[0]->key->string());
    EXPECT_EQ("host", object->properties[1]->key->string());
    EXPECT_EQ("port", object->properties[2]->key->string());

    const auto& next = std::get<std::unique_ptr<VariableAssgn>>(file->body[1]->stmt);
    ASSERT_NE(next, nullptr);
    EXPECT_EQ("42", next->init->string());
}

TEST(FluxParserTest, RecoversFromInvalidDictElement) {
    const std::string source = R"(
lookup = ["cpu": 1,, "mem": 2]
next = 42
)";

    Parser parser(source);
    auto file = parser.parse_file("invalid_dict.flux");

    ASSERT_NE(file, nullptr);
    ASSERT_FALSE(parser.errors().empty());
    ASSERT_EQ(2, file->body.size());

    const auto& lookup = std::get<std::unique_ptr<VariableAssgn>>(file->body[0]->stmt);
    ASSERT_NE(lookup, nullptr);
    ASSERT_NE(lookup->init, nullptr);
    ASSERT_EQ(Expression::Type::DictExpr, lookup->init->type);

    const auto& dict = std::get<std::unique_ptr<DictExpr>>(lookup->init->expr);
    ASSERT_NE(dict, nullptr);
    ASSERT_EQ(2, dict->elements.size());
    EXPECT_EQ("cpu", dict->elements[0]->key->string());
    EXPECT_EQ("1", dict->elements[0]->val->string());
    EXPECT_EQ("mem", dict->elements[1]->key->string());
    EXPECT_EQ("2", dict->elements[1]->val->string());

    const auto& next = std::get<std::unique_ptr<VariableAssgn>>(file->body[1]->stmt);
    ASSERT_NE(next, nullptr);
    EXPECT_EQ("42", next->init->string());
}

TEST(FluxParserTest, RecoversFromMissingConditionalConsequent) {
    const std::string source = R"(
status = if exists ready then else "bad"
)";

    Parser parser(source);
    auto file = parser.parse_file("missing_consequent.flux");

    ASSERT_NE(file, nullptr);
    ASSERT_FALSE(parser.errors().empty());
    ASSERT_EQ(1, file->body.size());

    const auto& status = std::get<std::unique_ptr<VariableAssgn>>(file->body[0]->stmt);
    ASSERT_NE(status, nullptr);
    ASSERT_NE(status->init, nullptr);
    ASSERT_EQ(Expression::Type::ConditionalExpr, status->init->type);

    const auto& cond = std::get<std::unique_ptr<ConditionalExpr>>(status->init->expr);
    ASSERT_NE(cond, nullptr);
    ASSERT_NE(cond->consequent, nullptr);
    ASSERT_NE(cond->alternate, nullptr);
    EXPECT_EQ(Expression::Type::BadExpr, cond->consequent->type);
    EXPECT_EQ(Expression::Type::StringLit, cond->alternate->type);

}

TEST(FluxParserTest, RecoversFromMissingConditionalAlternate) {
    const std::string source = R"(
status = if exists ready then "ok" else
)";

    Parser parser(source);
    auto file = parser.parse_file("missing_alternate.flux");

    ASSERT_NE(file, nullptr);
    ASSERT_FALSE(parser.errors().empty());
    ASSERT_EQ(1, file->body.size());

    const auto& status = std::get<std::unique_ptr<VariableAssgn>>(file->body[0]->stmt);
    ASSERT_NE(status, nullptr);
    ASSERT_NE(status->init, nullptr);
    ASSERT_EQ(Expression::Type::ConditionalExpr, status->init->type);

    const auto& cond = std::get<std::unique_ptr<ConditionalExpr>>(status->init->expr);
    ASSERT_NE(cond, nullptr);
    ASSERT_NE(cond->consequent, nullptr);
    ASSERT_NE(cond->alternate, nullptr);
    EXPECT_EQ(Expression::Type::StringLit, cond->consequent->type);
    EXPECT_EQ(Expression::Type::BadExpr, cond->alternate->type);

}

TEST(FluxParserTest, DumpsBadConditionalBranchesWithLocations) {
    const std::string source = R"(
status = if exists ready then else
)";

    Parser parser(source);
    auto file = parser.parse_file("bad_conditional_dump.flux");

    ASSERT_NE(file, nullptr);
    ASSERT_FALSE(parser.errors().empty());

    const std::string tree = dump_ast(*file);
    EXPECT_NE(tree.find("ConditionalExpr"), std::string::npos);
    EXPECT_NE(tree.find("Consequent"), std::string::npos);
    EXPECT_NE(tree.find("Alternate"), std::string::npos);
    EXPECT_NE(tree.find("BadExpr text=\""), std::string::npos);
    EXPECT_NE(tree.find(" loc="), std::string::npos);

    const std::string json = dump_ast_json(*file);
    EXPECT_NE(json.find("\"type\":\"Consequent\""), std::string::npos);
    EXPECT_NE(json.find("\"type\":\"Alternate\""), std::string::npos);
    EXPECT_NE(json.find("\"type\":\"BadExpr\""), std::string::npos);
    EXPECT_NE(json.find(" loc="), std::string::npos);
}

TEST(FluxParserTest, StopsConditionalRecoveryAtNextLineStatementBoundary) {
    const std::string source = R"(
status = if exists ready then "ok" else
next = 42
)";

    Parser parser(source);
    auto file = parser.parse_file("conditional_boundary.flux");

    ASSERT_NE(file, nullptr);
    ASSERT_FALSE(parser.errors().empty());
    ASSERT_EQ(2, file->body.size());

    const auto& status = std::get<std::unique_ptr<VariableAssgn>>(file->body[0]->stmt);
    ASSERT_NE(status, nullptr);
    ASSERT_NE(status->init, nullptr);
    ASSERT_EQ(Expression::Type::ConditionalExpr, status->init->type);

    const auto& cond = std::get<std::unique_ptr<ConditionalExpr>>(status->init->expr);
    ASSERT_NE(cond, nullptr);
    ASSERT_NE(cond->alternate, nullptr);
    EXPECT_EQ(Expression::Type::BadExpr, cond->alternate->type);

    const auto& next = std::get<std::unique_ptr<VariableAssgn>>(file->body[1]->stmt);
    ASSERT_NE(next, nullptr);
    EXPECT_EQ("next", next->id->name);
    EXPECT_EQ("42", next->init->string());
}

} // namespace
} // namespace pl
