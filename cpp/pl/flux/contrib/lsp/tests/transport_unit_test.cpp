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

#include <cstdio>
#include <cstring>
#include <gtest/gtest.h>
#include <optional>
#include <string>

#include "cpp/pl/flux/contrib/lsp/transport.h"

namespace {

// 向 FILE* 写入一段模拟 LSP 消息（header + body）
// 方便在 tmpfile 上复现真实的 LSP wire protocol 数据
void write_lsp_message(FILE* fp, const std::string& body) {
    std::fprintf(fp, "Content-Length: %zu\r\n\r\n%s", body.size(), body.c_str());
    std::fflush(fp);
    // 回到文件开头以便后续读取
    std::rewind(fp);
}

} // namespace

// ==================== read_message ====================

TEST(StdioTransportTest, ReadMessageCorrectParsesContentLength) {
    // 验证能正确读取标准的 LSP wire format 消息
    FILE* in = std::tmpfile();
    ASSERT_NE(in, nullptr);

    const std::string json_body = R"({"jsonrpc":"2.0","id":1,"result":42})";
    write_lsp_message(in, json_body);

    pl::flux::lsp::StdioTransport transport(in, nullptr);
    auto result = transport.read_message();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), json_body);
    std::fclose(in);
}

TEST(StdioTransportTest, ReadMessageReturnsNulloptOnEof) {
    // EOF 时应返回 nullopt，避免调用方误认为收到了空消息
    FILE* in = std::tmpfile();
    ASSERT_NE(in, nullptr);
    // 空文件，首次 fgets 即返回 nullptr

    pl::flux::lsp::StdioTransport transport(in, nullptr);
    auto result = transport.read_message();

    EXPECT_FALSE(result.has_value());
    std::fclose(in);
}

TEST(StdioTransportTest, ReadMessageReturnsNulloptOnBadContentLength) {
    // Content-Length 值非数字时无法确定 body 长度，必须返回 nullopt
    FILE* in = std::tmpfile();
    ASSERT_NE(in, nullptr);

    // 写入格式错误的 Content-Length
    std::fprintf(in, "Content-Length: abc\r\n\r\nhello");
    std::fflush(in);
    std::rewind(in);

    pl::flux::lsp::StdioTransport transport(in, nullptr);
    auto result = transport.read_message();

    EXPECT_FALSE(result.has_value());
    std::fclose(in);
}

TEST(StdioTransportTest, ReadMessageReturnsNulloptOnMissingContentLength) {
    // 没有 Content-Length header 时无法知道读取多少字节，必须返回 nullopt
    FILE* in = std::tmpfile();
    ASSERT_NE(in, nullptr);

    // 只有空行终止 header，没有 Content-Length
    std::fprintf(in, "\r\nhello");
    std::fflush(in);
    std::rewind(in);

    pl::flux::lsp::StdioTransport transport(in, nullptr);
    auto result = transport.read_message();

    EXPECT_FALSE(result.has_value());
    std::fclose(in);
}

TEST(StdioTransportTest, ReadMessageReturnsNulloptOnShortBody) {
    // Content-Length 声明 100 字节但实际不足，fread 读不够，应返回 nullopt
    FILE* in = std::tmpfile();
    ASSERT_NE(in, nullptr);

    std::fprintf(in, "Content-Length: 100\r\n\r\nshort");
    std::fflush(in);
    std::rewind(in);

    pl::flux::lsp::StdioTransport transport(in, nullptr);
    auto result = transport.read_message();

    EXPECT_FALSE(result.has_value());
    std::fclose(in);
}

TEST(StdioTransportTest, ReadMessageAcceptsHeaderCaseAndWhitespaceVariants) {
    FILE* in = std::tmpfile();
    ASSERT_NE(in, nullptr);

    const std::string json_body = R"({"variant":true})";
    std::fprintf(in, "content-length:\t%zu  \r\n\r\n%s", json_body.size(), json_body.c_str());
    std::fflush(in);
    std::rewind(in);

    pl::flux::lsp::StdioTransport transport(in, nullptr);
    auto result = transport.read_message();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), json_body);
    std::fclose(in);
}

// ==================== write_message ====================

TEST(StdioTransportTest, WriteMessageFormatsCorrectly) {
    // 验证输出格式严格遵循 LSP spec: "Content-Length: N\r\n\r\n{json}"
    FILE* out = std::tmpfile();
    ASSERT_NE(out, nullptr);

    pl::flux::lsp::StdioTransport transport(nullptr, out);
    const std::string json_body = R"({"jsonrpc":"2.0","method":"initialized"})";
    transport.write_message(json_body);

    // 读回写入的内容
    std::rewind(out);
    char buf[512] = {};
    std::fread(buf, 1, sizeof(buf) - 1, out);
    std::fclose(out);

    const std::string expected =
        "Content-Length: " + std::to_string(json_body.size()) + "\r\n\r\n" + json_body;
    EXPECT_EQ(std::string(buf), expected);
}

TEST(StdioTransportTest, WriteMessageEmptyJson) {
    // 空 JSON 字符串也是合法消息体（某些 notification 场景可能出现）
    FILE* out = std::tmpfile();
    ASSERT_NE(out, nullptr);

    pl::flux::lsp::StdioTransport transport(nullptr, out);
    transport.write_message("");

    std::rewind(out);
    char buf[128] = {};
    std::fread(buf, 1, sizeof(buf) - 1, out);
    std::fclose(out);

    EXPECT_EQ(std::string(buf), "Content-Length: 0\r\n\r\n");
}

// ==================== Move 语义 ====================

TEST(StdioTransportTest, MovedFromSourceReadsNullopt) {
    // move 后源对象的 in_ 被置 nullptr，read_message 必须返回 nullopt
    // 这是 move 语义的正确实现：避免两个对象竞争同一个 FILE*
    FILE* in = std::tmpfile();
    ASSERT_NE(in, nullptr);
    FILE* out = std::tmpfile();
    ASSERT_NE(out, nullptr);

    pl::flux::lsp::StdioTransport source(in, out);
    pl::flux::lsp::StdioTransport dest(std::move(source));

    // move 后源对象不应再访问已移交的 FILE*
    auto result = source.read_message();
    EXPECT_FALSE(result.has_value());

    // dest 应能正常工作
    const std::string json_body = R"({"ok":true})";
    write_lsp_message(in, json_body);
    auto dest_result = dest.read_message();
    ASSERT_TRUE(dest_result.has_value());
    EXPECT_EQ(dest_result.value(), json_body);

    std::fclose(in);
    std::fclose(out);
}

TEST(StdioTransportTest, MoveAssignmentTransfersOwnership) {
    // 验证 move 赋值运算符也正确转移 FILE* 所有权
    FILE* in1 = std::tmpfile();
    ASSERT_NE(in1, nullptr);
    FILE* out1 = std::tmpfile();
    ASSERT_NE(out1, nullptr);
    FILE* in2 = std::tmpfile();
    ASSERT_NE(in2, nullptr);
    FILE* out2 = std::tmpfile();
    ASSERT_NE(out2, nullptr);

    pl::flux::lsp::StdioTransport a(in1, out1);
    pl::flux::lsp::StdioTransport b(in2, out2);

    a = std::move(b);

    // b 被 move 后 read_message 应返回 nullopt
    auto result = b.read_message();
    EXPECT_FALSE(result.has_value());

    // a 现在持有 in2/out2
    const std::string json_body = R"({"moved":true})";
    write_lsp_message(in2, json_body);
    auto a_result = a.read_message();
    ASSERT_TRUE(a_result.has_value());
    EXPECT_EQ(a_result.value(), json_body);

    std::fclose(in1);
    std::fclose(out1);
    std::fclose(in2);
    std::fclose(out2);
}
