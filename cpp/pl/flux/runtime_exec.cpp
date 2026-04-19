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

#include "cpp/pl/flux/runtime_exec.h"

#include "absl/strings/str_cat.h"
#include "cpp/pl/flux/runtime_builtin.h"
#include "cpp/pl/flux/runtime_eval.h"

namespace pl {
namespace {

absl::StatusOr<std::string> property_name(const PropertyKey& key) {
    switch (key.type) {
        case PropertyKey::Type::Identifier:
            return std::get<std::unique_ptr<Identifier>>(key.key)->name;
        case PropertyKey::Type::StringLiteral:
            return std::get<std::unique_ptr<StringLit>>(key.key)->value;
    }
}

absl::StatusOr<std::string> flatten_member_name(const MemberExpr& member) {
    std::string suffix;
    auto property_or = property_name(*member.property);
    if (!property_or.ok()) {
        return property_or.status();
    }
    suffix = *property_or;

    const Expression* cursor = member.object.get();
    while (cursor->type == Expression::Type::MemberExpr) {
        const auto& inner = std::get<std::unique_ptr<MemberExpr>>(cursor->expr);
        auto inner_name_or = property_name(*inner->property);
        if (!inner_name_or.ok()) {
            return inner_name_or.status();
        }
        inner_name_or->append(".").append(suffix);
        suffix = std::move(*inner_name_or);
        cursor = inner->object.get();
    }

    if (cursor->type != Expression::Type::Identifier) {
        return absl::InvalidArgumentError("member assignment root must be an identifier");
    }
    const auto& root = std::get<std::unique_ptr<Identifier>>(cursor->expr);
    return root->name + "." + suffix;
}

absl::StatusOr<ExecutionResult> execute_option_assignment(const Assignment& assignment,
                                                          Environment& env) {
    switch (assignment.type) {
        case Assignment::Type::VariableAssignment: {
            const auto& var = std::get<std::unique_ptr<VariableAssgn>>(assignment.value);
            auto value_or = ExpressionEvaluator::Evaluate(*var->init, env);
            if (!value_or.ok()) {
                return value_or.status();
            }
            env.define_option(var->id->name, *value_or);
            return ExecutionResult::normal(*value_or);
        }
        case Assignment::Type::MemberAssignment: {
            const auto& member = std::get<std::unique_ptr<MemberAssgn>>(assignment.value);
            auto path_or = flatten_member_name(*member->member);
            if (!path_or.ok()) {
                return path_or.status();
            }
            auto value_or = ExpressionEvaluator::Evaluate(*member->init, env);
            if (!value_or.ok()) {
                return value_or.status();
            }
            env.define_option(*path_or, *value_or);
            return ExecutionResult::normal(*value_or);
        }
    }
}

std::string import_binding_name(const ImportDeclaration& import) {
    if (import.alias != nullptr) {
        return import.alias->name;
    }
    return import.path->value;
}

absl::StatusOr<Value> import_binding_value(const ImportDeclaration& import) {
    auto value_or = BuiltinRegistry::ImportPackage(import.path->value);
    if (!value_or.ok()) {
        return value_or.status();
    }
    if (import.alias == nullptr) {
        return *value_or;
    }
    auto props = value_or->as_object().properties;
    if (import.alias != nullptr) {
        props.emplace_back("alias", Value::string(import.alias->name));
    }
    return Value::object(std::move(props));
}

Value testcase_result_value(const TestCaseStmt& testcase, const ExecutionResult& result) {
    std::vector<std::pair<std::string, Value>> properties;
    properties.emplace_back("name", Value::string(testcase.id->name));
    properties.emplace_back("success", Value::boolean(true));
    if (testcase.extends != nullptr) {
        properties.emplace_back("extends", Value::string(testcase.extends->value));
    }
    properties.emplace_back("value", result.value);
    return Value::object(std::move(properties));
}

std::string statement_result_name(const Statement& stmt) {
    switch (stmt.type) {
        case Statement::Type::ExpressionStatement:
            return "_result";
        case Statement::Type::VariableAssignment: {
            const auto& var = std::get<std::unique_ptr<VariableAssgn>>(stmt.stmt);
            return var->id->name;
        }
        case Statement::Type::OptionStatement: {
            const auto& option = std::get<std::unique_ptr<OptionStmt>>(stmt.stmt);
            switch (option->assignment->type) {
                case Assignment::Type::VariableAssignment: {
                    const auto& var =
                        std::get<std::unique_ptr<VariableAssgn>>(option->assignment->value);
                    return "option." + var->id->name;
                }
                case Assignment::Type::MemberAssignment: {
                    const auto& member =
                        std::get<std::unique_ptr<MemberAssgn>>(option->assignment->value);
                    auto path_or = flatten_member_name(*member->member);
                    if (!path_or.ok()) {
                        return "option";
                    }
                    return "option." + *path_or;
                }
            }
        }
        case Statement::Type::BuiltinStatement: {
            const auto& builtin = std::get<std::unique_ptr<BuiltinStmt>>(stmt.stmt);
            return "builtin." + builtin->id->name;
        }
        case Statement::Type::TestCaseStatement: {
            const auto& testcase = std::get<std::unique_ptr<TestCaseStmt>>(stmt.stmt);
            return "testcase." + testcase->id->name;
        }
        case Statement::Type::ReturnStatement:
            return "return";
        case Statement::Type::BadStatement:
            return "bad";
    }
}

std::string resolved_result_name(const Statement& stmt, const Value& value) {
    if (value.type() == Value::Type::Table && value.as_table().result_name.has_value()) {
        return *value.as_table().result_name;
    }
    return statement_result_name(stmt);
}

void append_named_result(const Statement& stmt,
                         const ExecutionResult& exec_result,
                         FileExecutionResult& file_result) {
    if (exec_result.type != ExecutionResult::Type::Normal || exec_result.value.is_null()) {
        return;
    }
    file_result.results.push_back(NamedResult{.name = resolved_result_name(stmt, exec_result.value),
                                              .value = exec_result.value});
}

absl::StatusOr<ExecutionResult> execute_testcase_statement(const TestCaseStmt& testcase,
                                                           Environment& env) {
    auto result_or = StatementExecutor::ExecuteBlock(*testcase.block, env);
    if (!result_or.ok()) {
        return result_or.status();
    }
    auto testcase_value = testcase_result_value(testcase, *result_or);
    env.define_option("__flux.testcase." + testcase.id->name, testcase_value);
    return ExecutionResult::normal(std::move(testcase_value));
}

} // namespace

absl::StatusOr<ExecutionResult> StatementExecutor::Execute(const Statement& stmt,
                                                           Environment& env) {
    switch (stmt.type) {
        case Statement::Type::ExpressionStatement: {
            const auto& expr = std::get<std::unique_ptr<ExprStmt>>(stmt.stmt);
            auto value_or = ExpressionEvaluator::Evaluate(*expr->expression, env);
            if (!value_or.ok()) {
                return value_or.status();
            }
            return ExecutionResult::normal(*value_or);
        }
        case Statement::Type::VariableAssignment: {
            const auto& var = std::get<std::unique_ptr<VariableAssgn>>(stmt.stmt);
            auto value_or = ExpressionEvaluator::Evaluate(*var->init, env);
            if (!value_or.ok()) {
                return value_or.status();
            }
            env.define(var->id->name, *value_or);
            return ExecutionResult::normal(*value_or);
        }
        case Statement::Type::OptionStatement: {
            const auto& option = std::get<std::unique_ptr<OptionStmt>>(stmt.stmt);
            return execute_option_assignment(*option->assignment, env);
        }
        case Statement::Type::ReturnStatement: {
            const auto& ret = std::get<std::unique_ptr<ReturnStmt>>(stmt.stmt);
            auto value_or = ExpressionEvaluator::Evaluate(*ret->argument, env);
            if (!value_or.ok()) {
                return value_or.status();
            }
            return ExecutionResult::returned(*value_or);
        }
        case Statement::Type::BadStatement:
            return absl::InvalidArgumentError(
                absl::StrCat("cannot execute bad statement: ",
                             std::get<std::unique_ptr<BadStmt>>(stmt.stmt)->text));
        case Statement::Type::TestCaseStatement:
            return execute_testcase_statement(*std::get<std::unique_ptr<TestCaseStmt>>(stmt.stmt),
                                              env);
        case Statement::Type::BuiltinStatement: {
            const auto& builtin = std::get<std::unique_ptr<BuiltinStmt>>(stmt.stmt);
            auto status = BuiltinRegistry::Ensure(env, builtin->id->name);
            if (!status.ok()) {
                return status;
            }
            auto value_or = env.lookup(builtin->id->name);
            if (!value_or.ok()) {
                return value_or.status();
            }
            return ExecutionResult::normal(*value_or);
        }
    }
}

absl::StatusOr<ExecutionResult> StatementExecutor::ExecuteBlock(const Block& block,
                                                                Environment& env) {
    auto block_env = std::make_shared<Environment>(std::make_shared<Environment>(env));
    Environment local(block_env);
    ExecutionResult last = ExecutionResult::normal();
    for (const auto& stmt : block.body) {
        auto result_or = Execute(*stmt, local);
        if (!result_or.ok()) {
            return result_or.status();
        }
        last = *result_or;
        if (last.type == ExecutionResult::Type::Return) {
            return last;
        }
    }
    return last;
}

absl::StatusOr<FileExecutionResult> StatementExecutor::ExecuteFile(const File& file,
                                                                   Environment& env) {
    FileExecutionResult result;
    if (file.package != nullptr) {
        result.package_name = file.package->name->name;
        env.define_option("__flux.package", Value::string(result.package_name));
    }
    for (const auto& import : file.imports) {
        auto name = import_binding_name(*import);
        auto value_or = import_binding_value(*import);
        if (!value_or.ok()) {
            return value_or.status();
        }
        env.define(name, *value_or);
        result.imports.push_back(name);
    }
    for (const auto& stmt : file.body) {
        auto result_or = Execute(*stmt, env);
        if (!result_or.ok()) {
            return result_or.status();
        }
        result.last = *result_or;
        append_named_result(*stmt, result.last, result);
        if (result.last.type == ExecutionResult::Type::Return) {
            return absl::InvalidArgumentError("return statement is not allowed at file scope");
        }
    }
    return result;
}

} // namespace pl
