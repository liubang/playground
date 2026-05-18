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

#include <gtest/gtest.h>

#include "cpp/pl/flux/contrib/lsp/json_util.h"

using pl::flux::lsp::json_escape;
using pl::flux::lsp::json_escape_raw;

// ============ json_escape ============

// 最简单的路径，确认外层引号正常添加
TEST(JsonEscapeTest, PlainString) {
    EXPECT_EQ(json_escape("hello"), R"("hello")");
}

// JSON 字符串内双引号必须转义，否则破坏结构
TEST(JsonEscapeTest, ContainsQuote) {
    EXPECT_EQ(json_escape(R"(say "hi")"), R"("say \"hi\"")");
}

// 反斜杠是转义前缀，自身必须先转义，避免与后续字符组合成非法转义序列
TEST(JsonEscapeTest, ContainsBackslash) {
    EXPECT_EQ(json_escape(R"(C:\path)"), R"("C:\\path")");
}

// 换行和 tab 是 JSON 规范明确定义的短转义形式
TEST(JsonEscapeTest, NewlineAndTab) {
    EXPECT_EQ(json_escape("line1\nline2\tindented"), R"("line1\nline2\tindented")");
}

// 0x01 等控制字符不在 JSON 规范的短转义列表中，必须用 \uXXXX 表示
TEST(JsonEscapeTest, ControlCharacter) {
    std::string input;
    input.push_back(0x01);
    EXPECT_EQ(json_escape(input), R"("\u0001")");
}

// 空输入仍需输出一对引号，表示空 JSON 字符串
TEST(JsonEscapeTest, EmptyString) {
    EXPECT_EQ(json_escape(""), R"("")");
}

// ============ json_escape_raw ============

// 不加外层引号，纯内容转义
TEST(JsonEscapeRawTest, PlainString) {
    EXPECT_EQ(json_escape_raw("hello"), "hello");
}

// 双引号仍需转义——可能被嵌入到更大的 JSON 字符串中
TEST(JsonEscapeRawTest, ContainsQuote) {
    EXPECT_EQ(json_escape_raw(R"(say "hi")"), R"(say \"hi\")");
}

// 反斜杠转义逻辑与 json_escape 一致
TEST(JsonEscapeRawTest, ContainsBackslash) {
    EXPECT_EQ(json_escape_raw(R"(C:\path)"), R"(C:\\path)");
}

// 换行/tab 的短转义形式不变
TEST(JsonEscapeRawTest, NewlineAndTab) {
    EXPECT_EQ(json_escape_raw("line1\nline2\tindented"), R"(line1\nline2\tindented)");
}

// 控制字符同样走 \uXXXX 路径
TEST(JsonEscapeRawTest, ControlCharacter) {
    std::string input;
    input.push_back(0x01);
    EXPECT_EQ(json_escape_raw(input), R"(\u0001)");
}

// 空输入无任何输出
TEST(JsonEscapeRawTest, EmptyString) {
    EXPECT_EQ(json_escape_raw(""), "");
}
