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
    EXPECT_NE(json.find("\"summary\":\"name=dump.json.flux\""), std::string::npos);
    EXPECT_NE(json.find("\"type\":\"PackageClause\""), std::string::npos);
    EXPECT_NE(json.find("\"summary\":\"name=metrics\""), std::string::npos);
    EXPECT_NE(json.find("\"type\":\"ImportDeclaration\""), std::string::npos);
    EXPECT_NE(json.find("\"summary\":\"path=\\\"array\\\"\""), std::string::npos);
    EXPECT_NE(json.find("\"type\":\"ConditionalExpr\""), std::string::npos);
    EXPECT_NE(json.find("\"type\":\"UnaryExpr\""), std::string::npos);
    EXPECT_NE(json.find("\"summary\":\"op=exists\""), std::string::npos);
}

} // namespace
} // namespace pl
