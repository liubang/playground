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

#include "cpp/pl/flux/runtime_eval.h"

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include <cmath>
#include <optional>
#include <regex>
#include <sstream>
#include <unordered_map>

namespace pl {
namespace {

std::string expr_tag(const Expression& expr) {
    if (expr.loc.is_valid()) {
        std::stringstream ss;
        ss << expr.loc;
        return ss.str();
    }
    return expr.string();
}

absl::Status unsupported(const Expression& expr, const std::string& what) {
    return absl::UnimplementedError(
        absl::StrCat("unsupported expression ", what, " at ", expr_tag(expr)));
}

absl::Status type_error(const Expression& expr, const std::string& detail) {
    return absl::InvalidArgumentError(
        absl::StrCat("invalid expression ", detail, " at ", expr_tag(expr)));
}

std::string string_payload(const Value& value) {
    if (value.type() == Value::Type::String) {
        return value.as_string();
    }
    return value.string();
}

absl::StatusOr<std::string> property_name(const PropertyKey& key) {
    switch (key.type) {
        case PropertyKey::Type::Identifier:
            return std::get<std::unique_ptr<Identifier>>(key.key)->name;
        case PropertyKey::Type::StringLiteral:
            return std::get<std::unique_ptr<StringLit>>(key.key)->value;
    }
}

absl::StatusOr<Value> eval_impl(const Expression& expr, const Environment& env);

absl::Status invalid_call(const Expression& expr, const std::string& detail) {
    return absl::InvalidArgumentError(
        absl::StrCat("invalid call ", detail, " at ", expr_tag(expr)));
}

absl::Status invalid_invoke(const Value& callee, const std::string& detail) {
    return absl::InvalidArgumentError(
        absl::StrCat("invalid runtime call ", detail, " on ", callee.string()));
}

absl::StatusOr<std::string> parameter_name(const Property& param) {
    return property_name(*param.key);
}

bool is_named_call_argument(const Expression& expr) {
    if (expr.type != Expression::Type::ObjectExpr) {
        return false;
    }
    const auto& object = std::get<std::unique_ptr<ObjectExpr>>(expr.expr);
    return object->with == nullptr;
}

absl::StatusOr<std::unordered_map<std::string, Value>>
eval_named_arguments(const Expression& expr, const Environment& env) {
    std::unordered_map<std::string, Value> named_args;
    const auto& object = std::get<std::unique_ptr<ObjectExpr>>(expr.expr);
    for (const auto& property : object->properties) {
        auto name_or = property_name(*property->key);
        if (!name_or.ok()) {
            return name_or.status();
        }
        if (property->value == nullptr) {
            return absl::InvalidArgumentError(
                absl::StrCat("missing value for named argument: ", *name_or));
        }
        auto value_or = eval_impl(*property->value, env);
        if (!value_or.ok()) {
            return value_or.status();
        }
        named_args[*name_or] = *value_or;
    }
    return named_args;
}

absl::StatusOr<Value> execute_function_body(const FunctionBody& body, Environment& env) {
    switch (body.type) {
        case FunctionBody::Type::Expression:
            return eval_impl(*std::get<std::unique_ptr<Expression>>(body.body), env);
        case FunctionBody::Type::Block: {
            const auto& block = std::get<std::unique_ptr<Block>>(body.body);
            Environment block_env(std::make_shared<Environment>(env));
            Value last = Value::null();
            for (const auto& stmt : block->body) {
                switch (stmt->type) {
                    case Statement::Type::ExpressionStatement: {
                        const auto& expr_stmt = std::get<std::unique_ptr<ExprStmt>>(stmt->stmt);
                        auto value_or = eval_impl(*expr_stmt->expression, block_env);
                        if (!value_or.ok()) {
                            return value_or.status();
                        }
                        last = *value_or;
                        break;
                    }
                    case Statement::Type::VariableAssignment: {
                        const auto& assignment =
                            std::get<std::unique_ptr<VariableAssgn>>(stmt->stmt);
                        auto value_or = eval_impl(*assignment->init, block_env);
                        if (!value_or.ok()) {
                            return value_or.status();
                        }
                        block_env.define(assignment->id->name, *value_or);
                        last = *value_or;
                        break;
                    }
                    case Statement::Type::OptionStatement: {
                        const auto& option = std::get<std::unique_ptr<OptionStmt>>(stmt->stmt);
                        if (option->assignment->type != Assignment::Type::VariableAssignment) {
                            return absl::UnimplementedError(
                                "unsupported option member assignment in function body");
                        }
                        const auto& assignment =
                            std::get<std::unique_ptr<VariableAssgn>>(option->assignment->value);
                        auto value_or = eval_impl(*assignment->init, block_env);
                        if (!value_or.ok()) {
                            return value_or.status();
                        }
                        block_env.define_option(assignment->id->name, *value_or);
                        last = *value_or;
                        break;
                    }
                    case Statement::Type::ReturnStatement: {
                        const auto& ret = std::get<std::unique_ptr<ReturnStmt>>(stmt->stmt);
                        return eval_impl(*ret->argument, block_env);
                    }
                    case Statement::Type::BadStatement:
                        return absl::InvalidArgumentError(
                            absl::StrCat("cannot execute bad statement: ",
                                         std::get<std::unique_ptr<BadStmt>>(stmt->stmt)->text));
                    case Statement::Type::BuiltinStatement:
                    case Statement::Type::TestCaseStatement:
                        return absl::UnimplementedError(
                            "unsupported statement in function body");
                }
            }
            return last;
        }
    }
}

absl::Status bind_function_arguments(const FunctionValue& function,
                                     const FunctionExpr& function_expr,
                                     const CallExpr& call,
                                     const Expression& whole_expr,
                                     const Environment& caller_env,
                                     Environment& function_env,
                                     const std::optional<Value>& pipe_value) {
    std::unordered_map<std::string, Value> named_args;
    const bool use_named_args =
        call.arguments.size() == 1 && is_named_call_argument(*call.arguments[0]);
    if (use_named_args) {
        auto named_or = eval_named_arguments(*call.arguments[0], caller_env);
        if (!named_or.ok()) {
            return named_or.status();
        }
        named_args = std::move(*named_or);
    }

    std::vector<Value> positional_args;
    if (!use_named_args) {
        positional_args.reserve(call.arguments.size());
        for (const auto& argument : call.arguments) {
            auto value_or = eval_impl(*argument, caller_env);
            if (!value_or.ok()) {
                return value_or.status();
            }
            positional_args.push_back(*value_or);
        }
    }

    size_t positional_index = 0;
    for (const auto& param : function_expr.params) {
        auto name_or = parameter_name(*param);
        if (!name_or.ok()) {
            return name_or.status();
        }
        const std::string& name = *name_or;
        if (param->value != nullptr && param->value->type == Expression::Type::PipeLit) {
            if (pipe_value.has_value()) {
                function_env.define(name, *pipe_value);
                continue;
            }
            if (use_named_args) {
                auto it = named_args.find(name);
                if (it != named_args.end()) {
                    function_env.define(name, it->second);
                    named_args.erase(it);
                    continue;
                }
            }
            return invalid_call(whole_expr, absl::StrCat("missing pipe argument `", name, "`"));
        }
        if (use_named_args) {
            auto it = named_args.find(name);
            if (it != named_args.end()) {
                function_env.define(name, it->second);
                named_args.erase(it);
                continue;
            }
        } else if (positional_index < positional_args.size()) {
            function_env.define(name, positional_args[positional_index++]);
            continue;
        }

        if (param->value != nullptr) {
            auto default_or = eval_impl(*param->value, *function.closure);
            if (!default_or.ok()) {
                return default_or.status();
            }
            function_env.define(name, *default_or);
            continue;
        }
        return invalid_call(whole_expr, absl::StrCat("missing argument `", name, "`"));
    }

    if (use_named_args) {
        if (!named_args.empty()) {
            return invalid_call(whole_expr,
                                absl::StrCat("unexpected named argument `",
                                             named_args.begin()->first, "`"));
        }
    } else if (positional_index != positional_args.size()) {
        return invalid_call(whole_expr, "too many positional arguments");
    }

    return absl::OkStatus();
}

absl::StatusOr<Value> eval_function_expr(const FunctionExpr& function, const Environment& env) {
    auto callable = std::make_shared<FunctionValue>();
    callable->kind = FunctionValue::Kind::User;
    callable->name = "<anonymous>";
    callable->user_function = &function;
    callable->closure = std::make_shared<Environment>(env);
    return Value::function(std::move(callable));
}

absl::Status bind_positional_arguments(const FunctionValue& function,
                                       const FunctionExpr& function_expr,
                                       const std::vector<Value>& positional_args,
                                       Environment& function_env) {
    size_t positional_index = 0;
    for (const auto& param : function_expr.params) {
        auto name_or = parameter_name(*param);
        if (!name_or.ok()) {
            return name_or.status();
        }
        const std::string& name = *name_or;
        if (positional_index < positional_args.size()) {
            function_env.define(name, positional_args[positional_index++]);
            continue;
        }
        if (param->value != nullptr && param->value->type == Expression::Type::PipeLit) {
            return invalid_invoke(Value::function(std::make_shared<FunctionValue>(function)),
                                  absl::StrCat("missing pipe argument `", name, "`"));
        }
        if (param->value != nullptr) {
            auto default_or = eval_impl(*param->value, *function.closure);
            if (!default_or.ok()) {
                return default_or.status();
            }
            function_env.define(name, *default_or);
            continue;
        }
        return invalid_invoke(Value::function(std::make_shared<FunctionValue>(function)),
                              absl::StrCat("missing argument `", name, "`"));
    }
    if (positional_index != positional_args.size()) {
        return invalid_invoke(Value::function(std::make_shared<FunctionValue>(function)),
                              "too many positional arguments");
    }
    return absl::OkStatus();
}

absl::StatusOr<Value> invoke_function(const FunctionValue& function,
                                      const CallExpr& call,
                                      const Expression& whole_expr,
                                      const Environment& env,
                                      const std::optional<Value>& pipe_value) {
    if (function.kind == FunctionValue::Kind::Builtin) {
        if (!function.builtin) {
            return invalid_call(whole_expr, "builtin function is missing callback");
        }
        std::vector<Value> args;
        if (pipe_value.has_value() && call.arguments.empty() &&
            function.pipe_param_name == "tables") {
            args.push_back(Value::object({{function.pipe_param_name, *pipe_value}}));
        } else if (pipe_value.has_value() && call.arguments.empty()) {
            args.push_back(*pipe_value);
        } else if (pipe_value.has_value() && call.arguments.size() == 1 &&
                   is_named_call_argument(*call.arguments[0]) &&
                   !function.pipe_param_name.empty()) {
            auto named_or = eval_named_arguments(*call.arguments[0], env);
            if (!named_or.ok()) {
                return named_or.status();
            }
            auto named_args = std::move(*named_or);
            named_args.try_emplace(function.pipe_param_name, *pipe_value);
            std::vector<std::pair<std::string, Value>> properties;
            properties.reserve(named_args.size());
            for (auto& [name, value] : named_args) {
                properties.emplace_back(name, value);
            }
            args.push_back(Value::object(std::move(properties)));
        } else {
            args.reserve(call.arguments.size());
            for (const auto& argument : call.arguments) {
                auto value_or = eval_impl(*argument, env);
                if (!value_or.ok()) {
                    return value_or.status();
                }
                args.push_back(*value_or);
            }
            if (pipe_value.has_value()) {
                return invalid_call(whole_expr, "cannot forward pipe input to builtin");
            }
        }
        return function.builtin(args);
    }

    if (function.user_function == nullptr || function.closure == nullptr) {
        return invalid_call(whole_expr, "user function is missing closure state");
    }

    Environment function_env(function.closure);
    auto bind_status = bind_function_arguments(
        function, *function.user_function, call, whole_expr, env, function_env, pipe_value);
    if (!bind_status.ok()) {
        return bind_status;
    }
    return execute_function_body(*function.user_function->body, function_env);
}

absl::StatusOr<Value> invoke_prepared_function(const FunctionValue& function,
                                               const std::vector<Value>& positional_args) {
    if (function.kind == FunctionValue::Kind::Builtin) {
        if (!function.builtin) {
            return invalid_invoke(Value::function(std::make_shared<FunctionValue>(function)),
                                  "builtin function is missing callback");
        }
        return function.builtin(positional_args);
    }
    if (function.user_function == nullptr || function.closure == nullptr) {
        return invalid_invoke(Value::function(std::make_shared<FunctionValue>(function)),
                              "user function is missing closure state");
    }
    Environment function_env(function.closure);
    auto bind_status =
        bind_positional_arguments(function, *function.user_function, positional_args, function_env);
    if (!bind_status.ok()) {
        return bind_status;
    }
    return execute_function_body(*function.user_function->body, function_env);
}

absl::StatusOr<Value> eval_call(const CallExpr& call,
                                const Environment& env,
                                const Expression& whole_expr) {
    auto callee_or = eval_impl(*call.callee, env);
    if (!callee_or.ok()) {
        return callee_or.status();
    }
    auto callee = *callee_or;
    if (callee.type() != Value::Type::Function) {
        return invalid_call(whole_expr, "callee must evaluate to a function");
    }
    return invoke_function(callee.as_function(), call, whole_expr, env, std::nullopt);
}

absl::StatusOr<Value> eval_pipe(const PipeExpr& pipe,
                                const Environment& env,
                                const Expression& whole_expr) {
    auto input_or = eval_impl(*pipe.argument, env);
    if (!input_or.ok()) {
        return input_or.status();
    }
    auto callee_or = eval_impl(*pipe.call->callee, env);
    if (!callee_or.ok()) {
        return callee_or.status();
    }
    if (callee_or->type() != Value::Type::Function) {
        return invalid_call(whole_expr, "pipe destination must evaluate to a function");
    }
    return invoke_function(callee_or->as_function(), *pipe.call, whole_expr, env, *input_or);
}

absl::StatusOr<Value> eval_member(const MemberExpr& member,
                                  const Environment& env,
                                  const Expression& whole_expr) {
    auto object_or = eval_impl(*member.object, env);
    if (!object_or.ok()) {
        return object_or.status();
    }
    auto object = *object_or;
    if (object.type() != Value::Type::Object) {
        return type_error(whole_expr, "member access requires an object");
    }
    auto name_or = property_name(*member.property);
    if (!name_or.ok()) {
        return name_or.status();
    }
    auto name = *name_or;
    const Value* value = object.as_object().lookup(name);
    if (value == nullptr) {
        return absl::NotFoundError(absl::StrCat("missing object property: ", name));
    }
    return *value;
}

absl::StatusOr<Value> eval_index(const IndexExpr& index,
                                 const Environment& env,
                                 const Expression& whole_expr) {
    auto target_or = eval_impl(*index.array, env);
    if (!target_or.ok()) {
        return target_or.status();
    }
    auto target = *target_or;
    auto key_or = eval_impl(*index.index, env);
    if (!key_or.ok()) {
        return key_or.status();
    }
    auto key = *key_or;

    if (target.type() == Value::Type::Array) {
        size_t pos = 0;
        if (key.type() == Value::Type::Int) {
            if (key.as_int() < 0) {
                return type_error(whole_expr, "array index must be non-negative");
            }
            pos = static_cast<size_t>(key.as_int());
        } else if (key.type() == Value::Type::UInt) {
            pos = static_cast<size_t>(key.as_uint());
        } else {
            return type_error(whole_expr, "array index must be int or uint");
        }
        if (pos >= target.as_array().elements.size()) {
            return absl::OutOfRangeError("array index out of range");
        }
        return target.as_array().elements[pos];
    }

    if (target.type() == Value::Type::Object) {
        if (key.type() != Value::Type::String) {
            return type_error(whole_expr, "object index must be string");
        }
        const Value* value = target.as_object().lookup(key.as_string());
        if (value == nullptr) {
            return absl::NotFoundError(absl::StrCat("missing object property: ", key.as_string()));
        }
        return *value;
    }

    return type_error(whole_expr, "index access requires an array or object");
}

absl::StatusOr<Value> eval_string_expr(const StringExpr& expr, const Environment& env) {
    std::string out;
    for (const auto& part : expr.parts) {
        switch (part->type) {
            case StringExprPart::Type::Text:
                out += std::get<std::unique_ptr<TextPart>>(part->part)->value;
                break;
            case StringExprPart::Type::Interpolated: {
                const auto& interpolated = std::get<std::unique_ptr<InterpolatedPart>>(part->part);
                auto value_or = eval_impl(*interpolated->expression, env);
                if (!value_or.ok()) {
                    return value_or.status();
                }
                auto value = *value_or;
                out += string_payload(value);
                break;
            }
        }
    }
    return Value::string(out);
}

absl::StatusOr<Value> eval_object_expr(const ObjectExpr& object,
                                       const Environment& env,
                                       const Expression& whole_expr) {
    std::vector<std::pair<std::string, Value>> props;
    if (object.with != nullptr && object.with->source != nullptr) {
        auto base_value_or = env.lookup(object.with->source->name);
        if (!base_value_or.ok()) {
            return base_value_or.status();
        }
        auto base_value = *base_value_or;
        if (base_value.type() != Value::Type::Object) {
            return type_error(whole_expr, "record update source must be an object");
        }
        props = base_value.as_object().properties;
    }

    for (const auto& property : object.properties) {
        auto name_or = property_name(*property->key);
        if (!name_or.ok()) {
            return name_or.status();
        }
        auto name = *name_or;
        if (property->value == nullptr) {
            return type_error(whole_expr, "object property is missing a value");
        }
        auto value_or = eval_impl(*property->value, env);
        if (!value_or.ok()) {
            return value_or.status();
        }
        auto value = *value_or;
        bool replaced = false;
        for (auto& [key, current] : props) {
            if (key == name) {
                current = value;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            props.emplace_back(name, value);
        }
    }
    return Value::object(std::move(props));
}

absl::StatusOr<Value> eval_unary(const UnaryExpr& unary,
                                 const Environment& env,
                                 const Expression& whole_expr) {
    if (unary.op == Operator::ExistsOperator) {
        auto result = eval_impl(*unary.argument, env);
        if (!result.ok() && result.status().code() == absl::StatusCode::kNotFound) {
            return Value::boolean(false);
        }
        if (!result.ok()) {
            return result.status();
        }
        return Value::boolean(!result->is_null());
    }

    auto value_or = eval_impl(*unary.argument, env);
    if (!value_or.ok()) {
        return value_or.status();
    }
    auto value = *value_or;
    switch (unary.op) {
        case Operator::NotOperator:
            if (value.type() != Value::Type::Bool) {
                return type_error(whole_expr, "`not` requires a boolean");
            }
            return Value::boolean(!value.as_bool());
        case Operator::SubtractionOperator:
            if (value.type() == Value::Type::Int) {
                return Value::integer(-value.as_int());
            }
            if (value.type() == Value::Type::Float) {
                return Value::floating(-value.as_float());
            }
            return type_error(whole_expr, "unary `-` requires int or float");
        default:
            return unsupported(whole_expr, "unary operator");
    }
}

absl::StatusOr<Value> eval_binary_numeric(const BinaryExpr& binary,
                                          const Value& left,
                                          const Value& right,
                                          const Expression& whole_expr) {
    const bool use_float = left.type() == Value::Type::Float || right.type() == Value::Type::Float;
    auto left_num = left.type() == Value::Type::Float ? left.as_float()
                    : left.type() == Value::Type::Int ? static_cast<double>(left.as_int())
                                                      : static_cast<double>(left.as_uint());
    auto right_num = right.type() == Value::Type::Float ? right.as_float()
                     : right.type() == Value::Type::Int ? static_cast<double>(right.as_int())
                                                        : static_cast<double>(right.as_uint());

    switch (binary.op) {
        case Operator::AdditionOperator:
            return use_float ? Value::floating(left_num + right_num)
                             : Value::integer(static_cast<int64_t>(left_num + right_num));
        case Operator::SubtractionOperator:
            return use_float ? Value::floating(left_num - right_num)
                             : Value::integer(static_cast<int64_t>(left_num - right_num));
        case Operator::MultiplicationOperator:
            return use_float ? Value::floating(left_num * right_num)
                             : Value::integer(static_cast<int64_t>(left_num * right_num));
        case Operator::DivisionOperator:
            if (right_num == 0) {
                return type_error(whole_expr, "division by zero");
            }
            return Value::floating(left_num / right_num);
        case Operator::ModuloOperator:
            if (use_float) {
                return type_error(whole_expr, "modulo requires integer operands");
            }
            if (static_cast<int64_t>(right_num) == 0) {
                return type_error(whole_expr, "modulo by zero");
            }
            return Value::integer(static_cast<int64_t>(left_num) % static_cast<int64_t>(right_num));
        case Operator::LessThanOperator:
            return Value::boolean(left_num < right_num);
        case Operator::LessThanEqualOperator:
            return Value::boolean(left_num <= right_num);
        case Operator::GreaterThanOperator:
            return Value::boolean(left_num > right_num);
        case Operator::GreaterThanEqualOperator:
            return Value::boolean(left_num >= right_num);
        default:
            return unsupported(whole_expr, "numeric binary operator");
    }
}

absl::StatusOr<Value> eval_binary(const BinaryExpr& binary,
                                  const Environment& env,
                                  const Expression& whole_expr) {
    auto left_or = eval_impl(*binary.left, env);
    if (!left_or.ok()) {
        return left_or.status();
    }
    auto left = *left_or;
    auto right_or = eval_impl(*binary.right, env);
    if (!right_or.ok()) {
        return right_or.status();
    }
    auto right = *right_or;

    const bool left_numeric = left.type() == Value::Type::Int || left.type() == Value::Type::UInt ||
                              left.type() == Value::Type::Float;
    const bool right_numeric = right.type() == Value::Type::Int ||
                               right.type() == Value::Type::UInt ||
                               right.type() == Value::Type::Float;
    if (left_numeric && right_numeric) {
        return eval_binary_numeric(binary, left, right, whole_expr);
    }

    switch (binary.op) {
        case Operator::AdditionOperator:
            if (left.type() == Value::Type::String && right.type() == Value::Type::String) {
                return Value::string(left.as_string() + right.as_string());
            }
            return type_error(whole_expr, "`+` requires numeric or string operands");
        case Operator::EqualOperator:
            return Value::boolean(left == right);
        case Operator::NotEqualOperator:
            return Value::boolean(left != right);
        case Operator::RegexpMatchOperator:
        case Operator::NotRegexpMatchOperator: {
            if (left.type() != Value::Type::String || right.type() != Value::Type::Regex) {
                return type_error(whole_expr, "regex match requires string =~ regex");
            }
            std::string pattern = right.as_regex().literal;
            if (pattern.size() >= 2 && pattern.front() == '/' && pattern.back() == '/') {
                pattern = pattern.substr(1, pattern.size() - 2);
            }
            bool matched = std::regex_search(left.as_string(), std::regex(pattern));
            return Value::boolean(binary.op == Operator::RegexpMatchOperator ? matched : !matched);
        }
        case Operator::StartsWithOperator:
            if (left.type() != Value::Type::String || right.type() != Value::Type::String) {
                return type_error(whole_expr, "`startswith` requires string operands");
            }
            return Value::boolean(left.as_string().starts_with(right.as_string()));
        case Operator::InOperator:
            if (right.type() != Value::Type::Array) {
                return type_error(whole_expr, "`in` requires an array on the right-hand side");
            }
            for (const auto& item : right.as_array().elements) {
                if (item == left) {
                    return Value::boolean(true);
                }
            }
            return Value::boolean(false);
        default:
            return unsupported(whole_expr, "binary operator");
    }
}

absl::StatusOr<Value> eval_logical(const LogicalExpr& logical,
                                   const Environment& env,
                                   const Expression& whole_expr) {
    auto left_or = eval_impl(*logical.left, env);
    if (!left_or.ok()) {
        return left_or.status();
    }
    auto left = *left_or;
    if (left.type() != Value::Type::Bool) {
        return type_error(whole_expr, "logical operators require boolean operands");
    }
    if (logical.op == LogicalOperator::AndOperator && !left.as_bool()) {
        return Value::boolean(false);
    }
    if (logical.op == LogicalOperator::OrOperator && left.as_bool()) {
        return Value::boolean(true);
    }
    auto right_or = eval_impl(*logical.right, env);
    if (!right_or.ok()) {
        return right_or.status();
    }
    auto right = *right_or;
    if (right.type() != Value::Type::Bool) {
        return type_error(whole_expr, "logical operators require boolean operands");
    }
    return Value::boolean(logical.op == LogicalOperator::AndOperator ? right.as_bool()
                                                                     : right.as_bool());
}

absl::StatusOr<Value> eval_impl(const Expression& expr, const Environment& env) {
    switch (expr.type) {
        case Expression::Type::Identifier:
            return env.lookup(std::get<std::unique_ptr<Identifier>>(expr.expr)->name);
        case Expression::Type::ArrayExpr: {
            std::vector<Value> elements;
            for (const auto& item : std::get<std::unique_ptr<ArrayExpr>>(expr.expr)->elements) {
                auto value_or = eval_impl(*item->expression, env);
                if (!value_or.ok()) {
                    return value_or.status();
                }
                elements.push_back(*value_or);
            }
            return Value::array(std::move(elements));
        }
        case Expression::Type::ObjectExpr:
            return eval_object_expr(*std::get<std::unique_ptr<ObjectExpr>>(expr.expr), env, expr);
        case Expression::Type::FunctionExpr:
            return eval_function_expr(*std::get<std::unique_ptr<FunctionExpr>>(expr.expr), env);
        case Expression::Type::MemberExpr:
            return eval_member(*std::get<std::unique_ptr<MemberExpr>>(expr.expr), env, expr);
        case Expression::Type::IndexExpr:
            return eval_index(*std::get<std::unique_ptr<IndexExpr>>(expr.expr), env, expr);
        case Expression::Type::CallExpr:
            return eval_call(*std::get<std::unique_ptr<CallExpr>>(expr.expr), env, expr);
        case Expression::Type::PipeExpr:
            return eval_pipe(*std::get<std::unique_ptr<PipeExpr>>(expr.expr), env, expr);
        case Expression::Type::BinaryExpr:
            return eval_binary(*std::get<std::unique_ptr<BinaryExpr>>(expr.expr), env, expr);
        case Expression::Type::UnaryExpr:
            return eval_unary(*std::get<std::unique_ptr<UnaryExpr>>(expr.expr), env, expr);
        case Expression::Type::LogicalExpr:
            return eval_logical(*std::get<std::unique_ptr<LogicalExpr>>(expr.expr), env, expr);
        case Expression::Type::ConditionalExpr: {
            const auto& conditional = std::get<std::unique_ptr<ConditionalExpr>>(expr.expr);
            auto test_or = eval_impl(*conditional->test, env);
            if (!test_or.ok()) {
                return test_or.status();
            }
            auto test = *test_or;
            if (test.type() != Value::Type::Bool) {
                return type_error(expr, "conditional test must evaluate to bool");
            }
            return test.as_bool() ? eval_impl(*conditional->consequent, env)
                                  : eval_impl(*conditional->alternate, env);
        }
        case Expression::Type::StringExpr:
            return eval_string_expr(*std::get<std::unique_ptr<StringExpr>>(expr.expr), env);
        case Expression::Type::ParenExpr:
            return eval_impl(*std::get<std::unique_ptr<ParenExpr>>(expr.expr)->expression, env);
        case Expression::Type::IntegerLit:
            return Value::integer(std::get<std::unique_ptr<IntegerLit>>(expr.expr)->value);
        case Expression::Type::FloatLit:
            return Value::floating(std::get<std::unique_ptr<FloatLit>>(expr.expr)->value);
        case Expression::Type::StringLit:
            return Value::string(std::get<std::unique_ptr<StringLit>>(expr.expr)->value);
        case Expression::Type::DurationLit:
            return Value::duration(std::get<std::unique_ptr<DurationLit>>(expr.expr)->string());
        case Expression::Type::UnsignedIntegerLit:
            return Value::uinteger(std::get<std::unique_ptr<UintLit>>(expr.expr)->value);
        case Expression::Type::BooleanLit:
            return Value::boolean(std::get<std::unique_ptr<BooleanLit>>(expr.expr)->value);
        case Expression::Type::DateTimeLit:
            return Value::time(std::get<std::unique_ptr<DateTimeLit>>(expr.expr)->string());
        case Expression::Type::RegexpLit:
            return Value::regex(std::get<std::unique_ptr<RegexpLit>>(expr.expr)->string());
        case Expression::Type::DictExpr:
        case Expression::Type::PipeLit:
        case Expression::Type::LabelLit:
        case Expression::Type::BadExpr:
            return unsupported(expr, "kind");
    }
}

} // namespace

absl::StatusOr<Value> ExpressionEvaluator::Evaluate(const Expression& expr,
                                                    const Environment& env) {
    return eval_impl(expr, env);
}

absl::StatusOr<Value> ExpressionEvaluator::Invoke(const Value& callee,
                                                  const std::vector<Value>& positional_args) {
    if (callee.type() != Value::Type::Function) {
        return invalid_invoke(callee, "callee must evaluate to a function");
    }
    return invoke_prepared_function(callee.as_function(), positional_args);
}

} // namespace pl
