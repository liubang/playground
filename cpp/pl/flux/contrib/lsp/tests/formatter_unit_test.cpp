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
#include <memory>
#include <string>

#include "cpp/pl/flux/contrib/lsp/formatter.h"
#include "cpp/pl/flux/syntax/parser.h"

namespace {

// 解析 Flux 源码并格式化，返回格式化结果
// 封装 Parser + Formatter 的调用链，避免每个 TEST 重复样板代码
std::string format_source(const std::string& source, pl::flux::lsp::FormatOptions opts = {}) {
    pl::flux::Parser parser(source);
    auto file = parser.parse_file("test.flux");
    pl::flux::lsp::Formatter formatter(opts);
    return formatter.format(*file);
}

} // namespace

// ==================== 简单变量赋值 ====================

TEST(FormatterTest, SimpleVariableAssignment) {
    // 最基础的语句，验证 "id = expr" 格式化后保持单行
    const std::string source = "x = 1";
    const std::string result = format_source(source);
    EXPECT_EQ(result, "x = 1\n");
}

TEST(FormatterTest, SimpleVariableAssignmentString) {
    // 字符串字面量的赋值
    const std::string source = R"(name = "hello")";
    const std::string result = format_source(source);
    EXPECT_EQ(result,
              R"(name = "hello")"
              "\n");
}

TEST(FormatterTest, MultipleSimpleAssignmentsNoExtraBlankLine) {
    // 连续的简单赋值不应插入额外空行（它们构成逻辑分组）
    const std::string source = "x = 1\ny = 2";
    const std::string result = format_source(source);
    EXPECT_EQ(result, "x = 1\ny = 2\n");
}

// ==================== Pipe chain ====================

TEST(FormatterTest, PipeChainFormatsWithNewlineAndIndent) {
    // Pipe chain 应在每个 |> 前换行并缩进，保持数据流的可读性
    const std::string source =
        "from(bucket: \"test\") |> filter(fn: (r) => r._value > 0) |> yield()";
    const std::string result = format_source(source);

    // 第一行是 from(...)，后续每个 |> 独占一行并缩进
    EXPECT_NE(result.find("from("), std::string::npos);
    EXPECT_NE(result.find("\n    |> filter("), std::string::npos);
    EXPECT_NE(result.find("\n    |> yield("), std::string::npos);
}

TEST(FormatterTest, PipeChainSingleStep) {
    // 单步 pipe 仍然要换行缩进，格式统一
    const std::string source = "data |> yield()";
    const std::string result = format_source(source);
    EXPECT_NE(result.find("\n    |> yield("), std::string::npos);
}

// ==================== Function expression ====================

TEST(FormatterTest, FunctionExprSimple) {
    // 简单箭头函数，单行表达
    const std::string source = "fn = (r) => r._value > 0";
    const std::string result = format_source(source);
    EXPECT_EQ(result, "fn = (r) => r._value > 0\n");
}

TEST(FormatterTest, FunctionExprWithMultipleParams) {
    // 多参数箭头函数
    const std::string source = "add = (a, b) => a + b";
    const std::string result = format_source(source);
    EXPECT_EQ(result, "add = (a, b) => a + b\n");
}

TEST(FormatterTest, FunctionExprWithBlockBody) {
    // Block body 的函数需要展开为多行
    const std::string source = "fn = (x) => {\n    return x + 1\n}";
    const std::string result = format_source(source);

    // 应包含花括号和换行
    EXPECT_NE(result.find("(x) => {"), std::string::npos);
    EXPECT_NE(result.find("return x + 1"), std::string::npos);
    EXPECT_NE(result.find('}'), std::string::npos);
}

// ==================== 空 Array / Object ====================

TEST(FormatterTest, EmptyArray) {
    // 空 array 字面量应格式化为 "[]"
    const std::string source = "arr = []";
    const std::string result = format_source(source);
    EXPECT_EQ(result, "arr = []\n");
}

TEST(FormatterTest, EmptyObject) {
    // 空 object 字面量应格式化为 "{}"
    const std::string source = "obj = {}";
    const std::string result = format_source(source);
    EXPECT_EQ(result, "obj = {}\n");
}

TEST(FormatterTest, EmptyDict) {
    // 空 dict 字面量应格式化为 "[:]"
    const std::string source = "d = [:]";
    const std::string result = format_source(source);
    EXPECT_EQ(result, "d = [:]\n");
}

// ==================== 超宽行自动换行 ====================

TEST(FormatterTest, LongCallExpressionWraps) {
    // 超过 max_line_width 的 call expression 应自动展开为多行
    // 使用较短的参数名，确保换行后每行可以在限制内
    const std::string source = "result = from(bucket: \"my-bucket\", org: \"my-org\", token: "
                               "\"secret-token-value\", start: -1h)";
    const std::string result = format_source(source, {.max_line_width = 40});

    // 展开后应包含换行和缩进，而非全部挤在一行
    EXPECT_NE(result.find('\n'), std::string::npos);
    // 验证每行不超过 max_line_width + 合理容差（缩进+逗号）
    size_t pos = 0;
    while (pos < result.size()) {
        auto eol = result.find('\n', pos);
        if (eol == std::string::npos) {
            eol = result.size();
        }
        auto line_len = eol - pos;
        // 容差：不可分割的 token（如字符串字面量）+缩进可能略超阈值
        EXPECT_LE(line_len, 60) << "行过长: [" << result.substr(pos, line_len) << "]";
        pos = eol + 1;
    }
}

TEST(FormatterTest, LongObjectExpressionWraps) {
    // 属性较多的 object 应展开为多行格式
    const std::string source = R"(obj = {a: "value-a", b: "value-b", c: "value-c", d: "value-d"})";
    const std::string result = format_source(source, {.max_line_width = 40});

    // 多行 object 格式：每条属性独占一行，带缩进和逗号
    EXPECT_NE(result.find("\n    a:"), std::string::npos);
    EXPECT_NE(result.find("\n    b:"), std::string::npos);
}

// ==================== Package 和 Import ====================

TEST(FormatterTest, PackageAndImports) {
    // package clause 和 import 声明的格式化
    const std::string source = "package mypkg\nimport \"csv\"\nimport \"http\"";
    const std::string result = format_source(source);

    EXPECT_NE(result.find("package mypkg\n\n"), std::string::npos);
    EXPECT_NE(result.find("import \"csv\"\n"), std::string::npos);
    EXPECT_NE(result.find("import \"http\"\n"), std::string::npos);
}

// ==================== Option 语句 ====================

TEST(FormatterTest, OptionStatement) {
    // option 语句格式化
    const std::string source = "option task = {name: \"demo\", every: 1h}";
    const std::string result = format_source(source);
    EXPECT_NE(result.find("option task = "), std::string::npos);
}

// ==================== 缩进选项 ====================

TEST(FormatterTest, CustomIndentWidth) {
    // 自定义缩进宽度应生效
    const std::string source = "data |> yield()";
    pl::flux::lsp::FormatOptions opts;
    opts.indent_width = 2;
    const std::string result = format_source(source, opts);

    // indent_width=2 时，pipe 行缩进应为 2 个空格
    EXPECT_NE(result.find("\n  |> yield("), std::string::npos);
}
