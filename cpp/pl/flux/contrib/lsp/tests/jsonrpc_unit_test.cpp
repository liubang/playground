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
// Created: 2026/05/18 11:26

#include <cstdint>
#include <gtest/gtest.h>
#include <optional>
#include <variant>

#include "cpp/pl/flux/contrib/lsp/jsonrpc.h"

using pl::flux::lsp::ErrorCode;
using pl::flux::lsp::JsonRpcError;
using pl::flux::lsp::JsonRpcMessage;
using pl::flux::lsp::make_error_response;
using pl::flux::lsp::make_notification;
using pl::flux::lsp::make_response;
using pl::flux::lsp::parse_message;

// ============ parse_message ============

// 正常的 JSON-RPC request，整数 id
TEST(ParseMessageTest, ValidRequestWithIntId) {
    auto msg = parse_message(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})");
    ASSERT_TRUE(msg.has_value());
    ASSERT_TRUE(msg->id.has_value());
    ASSERT_TRUE(std::holds_alternative<int64_t>(msg->id.value()));
    EXPECT_EQ(std::get<int64_t>(msg->id.value()), 1);
    EXPECT_EQ(msg->method, "initialize");
}

// 字符串 id 也是合法的 JSON-RPC id 类型
TEST(ParseMessageTest, ValidRequestWithStringId) {
    auto msg = parse_message(R"({"jsonrpc":"2.0","id":"abc","method":"shutdown","params":null})");
    ASSERT_TRUE(msg.has_value());
    ASSERT_TRUE(msg->id.has_value());
    ASSERT_TRUE(std::holds_alternative<std::string>(msg->id.value()));
    EXPECT_EQ(std::get<std::string>(msg->id.value()), "abc");
    EXPECT_EQ(msg->method, "shutdown");
}

// notification 没有 id 字段
TEST(ParseMessageTest, NotificationWithoutId) {
    auto msg = parse_message(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    ASSERT_TRUE(msg.has_value());
    EXPECT_FALSE(msg->id.has_value());
    EXPECT_EQ(msg->method, "initialized");
}

// method 是 JSON-RPC 2.0 必需字段，缺失应拒绝
TEST(ParseMessageTest, MissingMethodReturnsNullopt) {
    auto msg = parse_message(R"({"jsonrpc":"2.0","id":1,"params":{}})");
    EXPECT_FALSE(msg.has_value());
}

// 畸形 JSON 无法解析
TEST(ParseMessageTest, MalformedJsonReturnsNullopt) {
    auto msg = parse_message("{not valid json}");
    EXPECT_FALSE(msg.has_value());
}

// ============ make_response ============

// 验证成功响应的完整 JSON 结构
TEST(MakeResponseTest, IntIdResponseFormat) {
    std::string result = make_response(int64_t{42}, R"({"version":"2.0"})");
    EXPECT_NE(result.find(R"("jsonrpc":"2.0")"), std::string::npos);
    EXPECT_NE(result.find(R"("id":42)"), std::string::npos);
    EXPECT_NE(result.find(R"("result":{"version":"2.0"})"), std::string::npos);
}

// 字符串 id 在 JSON 中应以字符串形式出现
TEST(MakeResponseTest, StringIdResponseFormat) {
    std::string result = make_response(std::string{"my-id"}, R"("ok")");
    EXPECT_NE(result.find(R"("id":"my-id")"), std::string::npos);
    EXPECT_NE(result.find(R"("result":"ok")"), std::string::npos);
}

// ============ make_error_response ============

// 错误响应必须包含 error 对象及正确的 code/message
TEST(MakeErrorResponseTest, ContainsErrorCodeAndMessage) {
    JsonRpcError err{.code = ErrorCode::MethodNotFound, .message = "method not found"};
    std::string result = make_error_response(int64_t{1}, err);

    EXPECT_NE(result.find(R"("jsonrpc":"2.0")"), std::string::npos);
    EXPECT_NE(result.find(R"("id":1)"), std::string::npos);
    EXPECT_NE(result.find(R"("error")"), std::string::npos);
    EXPECT_NE(result.find("-32601"), std::string::npos);
    EXPECT_NE(result.find("method not found"), std::string::npos);
}

// 成功响应不应出现 error 字段——这里确认 error 响应没有 result 字段
TEST(MakeErrorResponseTest, NoResultFieldInError) {
    JsonRpcError err{.code = ErrorCode::InternalError, .message = "internal error"};
    std::string result = make_error_response(int64_t{2}, err);

    EXPECT_EQ(result.find(R"("result")"), std::string::npos);
}

// ============ make_notification ============

// notification 没有 id 字段，只有 method 和 params
TEST(MakeNotificationTest, FormatWithoutId) {
    std::string result =
        make_notification("textDocument/publishDiagnostics", R"({"diagnostics":[]})");

    EXPECT_NE(result.find(R"("jsonrpc":"2.0")"), std::string::npos);
    EXPECT_NE(result.find(R"("method":"textDocument/publishDiagnostics")"), std::string::npos);
    EXPECT_NE(result.find(R"("params":{"diagnostics":[]})"), std::string::npos);
    // notification 不应包含 id
    EXPECT_EQ(result.find(R"("id")"), std::string::npos);
}
