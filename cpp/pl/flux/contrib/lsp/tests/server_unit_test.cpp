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
#include <string>
#include <thread>

#include "cpp/pl/flux/contrib/lsp/json_util.h"
#include "cpp/pl/flux/contrib/lsp/server.h"
#include "cpp/pl/flux/contrib/lsp/transport.h"
#include "simdjson.h"

using namespace pl::flux::lsp;

// 向 FILE* 写入一条完整的 LSP 消息 (Content-Length header + body)
static void write_lsp_message(FILE* f, const std::string& json) {
    fprintf(f, "Content-Length: %zu\r\n\r\n%s", json.size(), json.c_str());
    fflush(f);
}

// 从 FILE* 读取一条 LSP 消息的 body: 解析 Content-Length header 后读取对应长度
static std::string read_lsp_response(FILE* f) {
    char line[256];
    int content_length = -1;
    while (fgets(line, sizeof(line), f)) {
        // header 与 body 之间以空行分隔
        if (strcmp(line, "\r\n") == 0 || strcmp(line, "\n") == 0) {
            break;
        }
        if (strncmp(line, "Content-Length: ", 16) == 0) {
            content_length = atoi(line + 16);
        }
    }
    if (content_length <= 0) {
        return "";
    }
    std::string body(static_cast<size_t>(content_length), '\0');
    fread(body.data(), 1, static_cast<size_t>(content_length), f);
    return body;
}

// 常用 JSON-RPC 消息模板

static std::string make_initialize_request(int id) {
    return R"({"jsonrpc":"2.0","id":)" + std::to_string(id) +
           R"(,"method":"initialize","params":{"capabilities":{}}})";
}

static std::string make_initialized_notification() {
    return R"({"jsonrpc":"2.0","method":"initialized","params":{}})";
}

static std::string make_shutdown_request(int id) {
    return R"({"jsonrpc":"2.0","id":)" + std::to_string(id) +
           R"(,"method":"shutdown","params":null})";
}

static std::string make_exit_notification() {
    return R"({"jsonrpc":"2.0","method":"exit","params":{}})";
}

static std::string make_did_open_notification(const std::string& uri, const std::string& text) {
    return R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":)" +
           json_escape(uri) + R"(,"languageId":"flux","version":0,"text":)" + json_escape(text) +
           R"(}}})";
}

static std::string make_completion_request(int id,
                                           const std::string& uri,
                                           int line,
                                           int character) {
    return R"({"jsonrpc":"2.0","id":)" + std::to_string(id) +
           R"(,"method":"textDocument/completion","params":{"textDocument":{"uri":")" + uri +
           R"("},"position":{"line":)" + std::to_string(line) + R"(,"character":)" +
           std::to_string(character) + "}}}";
}

static std::string make_hover_request(int id, const std::string& uri, int line, int character) {
    return R"({"jsonrpc":"2.0","id":)" + std::to_string(id) +
           R"(,"method":"textDocument/hover","params":{"textDocument":{"uri":")" + uri +
           R"("},"position":{"line":)" + std::to_string(line) + R"(,"character":)" +
           std::to_string(character) + "}}}";
}

static std::string make_formatting_request(int id, const std::string& uri) {
    return R"({"jsonrpc":"2.0","id":)" + std::to_string(id) +
           R"(,"method":"textDocument/formatting","params":{"textDocument":{"uri":")" + uri +
           R"("},"options":{"tabSize":4,"insertSpaces":true}}})";
}

static size_t count_occurrences(const std::string& text, const std::string& needle) {
    size_t count = 0;
    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

static void expect_valid_json(const std::string& json) {
    simdjson::dom::parser parser;
    auto parsed = parser.parse(json);
    EXPECT_FALSE(parsed.error()) << json;
}

// ---------- 测试用例 ----------

// 场景: 完成 initialize -> initialized 生命周期握手，验证返回的 capabilities
TEST(FluxLanguageServerTest, InitializeHandshake) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    // 预置握手消息 + exit 让 server 退出
    write_lsp_message(in_file, make_initialize_request(1));
    write_lsp_message(in_file, make_initialized_notification());
    write_lsp_message(in_file, make_shutdown_request(2));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);

    // 读取 initialize 的 response
    auto resp = read_lsp_response(out_file);
    ASSERT_FALSE(resp.empty());
    // 必须包含 capabilities 字段 (LSP 规范要求)
    EXPECT_NE(resp.find("\"capabilities\""), std::string::npos);
    // 必须包含 serverInfo
    EXPECT_NE(resp.find("\"serverInfo\""), std::string::npos);

    // 读取 shutdown 的 response
    auto shutdown_resp = read_lsp_response(out_file);
    ASSERT_FALSE(shutdown_resp.empty());
    EXPECT_NE(shutdown_resp.find("\"result\""), std::string::npos);

    fclose(in_file);
    fclose(out_file);
}

// 场景: 在 initialize 之前发送 completion request，server 应返回 ServerNotInitialized
TEST(FluxLanguageServerTest, RejectBeforeInitialized) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    // 直接发 completion (跳过握手)，然后 shutdown 退出
    write_lsp_message(in_file, make_completion_request(1, "file:///test.flux", 0, 0));
    write_lsp_message(in_file, make_shutdown_request(2));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);

    auto resp = read_lsp_response(out_file);
    ASSERT_FALSE(resp.empty());
    // LSP 规范: 未初始化时请求应返回 -32002
    EXPECT_NE(resp.find("-32002"), std::string::npos);

    fclose(in_file);
    fclose(out_file);
}

// 场景: 打开含语法错误的文档后，server 应推送 publishDiagnostics notification
TEST(FluxLanguageServerTest, DidOpenWithDiagnostics) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    write_lsp_message(in_file, make_initialize_request(1));
    write_lsp_message(in_file, make_initialized_notification());
    // "x = + ;" 是无效 Flux 语法，会触发 parser 报错
    write_lsp_message(in_file, make_did_open_notification("file:///bad.flux", "x = + ;"));
    write_lsp_message(in_file, make_shutdown_request(2));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);

    // 第一条是 initialize response
    auto init_resp = read_lsp_response(out_file);
    ASSERT_FALSE(init_resp.empty());
    EXPECT_NE(init_resp.find("\"capabilities\""), std::string::npos);

    // 第二条是 publishDiagnostics notification (server 主动推送)
    auto diag_resp = read_lsp_response(out_file);
    ASSERT_FALSE(diag_resp.empty());
    EXPECT_NE(diag_resp.find("textDocument/publishDiagnostics"), std::string::npos);
    EXPECT_NE(diag_resp.find("\"diagnostics\""), std::string::npos);

    fclose(in_file);
    fclose(out_file);
}

// 场景: 打开文档后请求 completion，验证返回非空 items
TEST(FluxLanguageServerTest, CompletionReturnsItems) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    write_lsp_message(in_file, make_initialize_request(1));
    write_lsp_message(in_file, make_initialized_notification());
    write_lsp_message(in_file, make_did_open_notification("file:///comp.flux", "x = 1\n"));
    // position (1, 0): 第二行开头(空行)，typing_prefix 为空，触发通用补全返回所有候选
    write_lsp_message(in_file, make_completion_request(2, "file:///comp.flux", 1, 0));
    write_lsp_message(in_file, make_shutdown_request(3));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);

    // 跳过 initialize response
    auto init_resp = read_lsp_response(out_file);
    ASSERT_FALSE(init_resp.empty());

    // 跳过 didOpen 触发的 publishDiagnostics notification
    auto diag_resp = read_lsp_response(out_file);

    // 读取 completion response
    auto comp_resp = read_lsp_response(out_file);
    ASSERT_FALSE(comp_resp.empty());
    // completion response 必须包含 items 数组
    EXPECT_NE(comp_resp.find("\"items\""), std::string::npos);
    // 至少应返回 Flux 关键字或内置函数
    EXPECT_NE(comp_resp.find("\"label\""), std::string::npos);

    fclose(in_file);
    fclose(out_file);
}

// 场景: 函数补全返回 snippet，占位参数可被编辑器展开
TEST(FluxLanguageServerTest, CompletionReturnsFunctionSnippets) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    write_lsp_message(in_file, make_initialize_request(1));
    write_lsp_message(in_file, make_initialized_notification());
    write_lsp_message(
        in_file, make_did_open_notification("file:///snip.flux", "myFunc = (x, y) => x + y\n"));
    write_lsp_message(in_file, make_completion_request(2, "file:///snip.flux", 1, 0));
    write_lsp_message(in_file, make_shutdown_request(3));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);
    read_lsp_response(out_file);
    read_lsp_response(out_file);

    auto comp_resp = read_lsp_response(out_file);
    ASSERT_FALSE(comp_resp.empty());
    expect_valid_json(comp_resp);
    EXPECT_NE(comp_resp.find(R"("insertTextFormat":2)"), std::string::npos) << comp_resp;
    EXPECT_NE(comp_resp.find(R"("insertText":"range(start: ${1:value}, stop: ${2:value})$0")"),
              std::string::npos)
        << comp_resp;
    EXPECT_NE(comp_resp.find(R"("insertText":"yield(name: ${1:value})$0")"), std::string::npos)
        << comp_resp;
    EXPECT_EQ(comp_resp.find(R"("insertText":"yield(?name:)"), std::string::npos) << comp_resp;
    EXPECT_NE(comp_resp.find(R"("insertText":"myFunc(${1:x}, ${2:y})$0")"), std::string::npos)
        << comp_resp;

    fclose(in_file);
    fclose(out_file);
}

// 场景: 打开文档后请求 formatting，验证返回 TextEdit 数组
TEST(FluxLanguageServerTest, FormattingReturnsEdits) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    // 故意使用不一致缩进，让 formatter 产生修改
    write_lsp_message(in_file, make_initialize_request(1));
    write_lsp_message(in_file, make_initialized_notification());
    write_lsp_message(in_file, make_did_open_notification("file:///fmt.flux", "x  =  1\ny=2"));
    write_lsp_message(in_file, make_formatting_request(2, "file:///fmt.flux"));
    write_lsp_message(in_file, make_shutdown_request(3));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);

    // 跳过 initialize response
    auto init_resp = read_lsp_response(out_file);
    ASSERT_FALSE(init_resp.empty());

    // 跳过 didOpen 触发的 publishDiagnostics
    auto diag_resp = read_lsp_response(out_file);

    // 读取 formatting response
    auto fmt_resp = read_lsp_response(out_file);
    ASSERT_FALSE(fmt_resp.empty());
    // formatting 应返回 TextEdit 数组 (可能为空数组 [] 或包含编辑)
    // 关键: 结果中必须包含 result 字段，且值为数组结构
    EXPECT_NE(fmt_resp.find("\"result\""), std::string::npos);

    fclose(in_file);
    fclose(out_file);
}

// 场景: 增量文档同步 (TextDocumentSyncKind = 2)
TEST(FluxLanguageServerTest, IncrementalSync) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    write_lsp_message(in_file, make_initialize_request(1));
    write_lsp_message(in_file, make_initialized_notification());
    write_lsp_message(in_file, make_did_open_notification("file:///inc.flux", "x = 1\ny = 2"));

    // Send incremental change: replace "1" with "42" (line 0, char 4-5)
    std::string incremental_change =
        R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{)"
        R"("textDocument":{"uri":"file:///inc.flux","version":1},)"
        R"("contentChanges":[{"range":{"start":{"line":0,"character":4},)"
        R"("end":{"line":0,"character":5}},"text":"42"}]}})";
    write_lsp_message(in_file, incremental_change);

    // Request completion to verify content was updated
    write_lsp_message(in_file, make_completion_request(2, "file:///inc.flux", 1, 0));
    write_lsp_message(in_file, make_shutdown_request(3));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);

    // init resp
    auto init_resp = read_lsp_response(out_file);
    ASSERT_FALSE(init_resp.empty());
    // check change:2 in capabilities (with space in JSON formatting)
    EXPECT_NE(init_resp.find("\"change\": 2"), std::string::npos) << init_resp;

    // didOpen diagnostics
    read_lsp_response(out_file);
    // didChange diagnostics
    read_lsp_response(out_file);

    // completion response should include user variable "x" (from AST)
    auto comp_resp = read_lsp_response(out_file);
    ASSERT_FALSE(comp_resp.empty());
    EXPECT_NE(comp_resp.find("\"items\""), std::string::npos);

    fclose(in_file);
    fclose(out_file);
}

// 场景: documentSymbol 返回文件中定义的符号
TEST(FluxLanguageServerTest, DocumentSymbol) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    write_lsp_message(in_file, make_initialize_request(1));
    write_lsp_message(in_file, make_initialized_notification());
    write_lsp_message(
        in_file, make_did_open_notification("file:///sym.flux", "x = 1\naddOne = (n) => n + 1"));

    // Request documentSymbol
    std::string symbol_req =
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file:///sym.flux"}}})";
    write_lsp_message(in_file, symbol_req);
    write_lsp_message(in_file, make_shutdown_request(3));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);

    // skip init resp
    read_lsp_response(out_file);
    // skip diagnostics
    read_lsp_response(out_file);

    // documentSymbol response
    auto sym_resp = read_lsp_response(out_file);
    ASSERT_FALSE(sym_resp.empty());
    // Should contain the variable "x"
    EXPECT_NE(sym_resp.find("\"x\""), std::string::npos) << sym_resp;
    // Should contain the function "addOne"
    EXPECT_NE(sym_resp.find("\"addOne\""), std::string::npos) << sym_resp;

    fclose(in_file);
    fclose(out_file);
}

// 场景: foldingRange 返回可折叠区域
TEST(FluxLanguageServerTest, FoldingRange) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    write_lsp_message(in_file, make_initialize_request(1));
    write_lsp_message(in_file, make_initialized_notification());
    // Multi-line function: foldable
    write_lsp_message(
        in_file,
        make_did_open_notification(
            "file:///fold.flux", "compute = (a, b) =>\n    a + b\n\nresult = compute(a: 1, b: 2)"));

    // Request foldingRange
    std::string fold_req =
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/foldingRange","params":{"textDocument":{"uri":"file:///fold.flux"}}})";
    write_lsp_message(in_file, fold_req);
    write_lsp_message(in_file, make_shutdown_request(3));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);

    // skip init resp
    read_lsp_response(out_file);
    // skip diagnostics
    read_lsp_response(out_file);

    // foldingRange response
    auto fold_resp = read_lsp_response(out_file);
    ASSERT_FALSE(fold_resp.empty());
    // Should contain at least one folding range with startLine
    EXPECT_NE(fold_resp.find("\"startLine\""), std::string::npos) << fold_resp;

    fclose(in_file);
    fclose(out_file);
}

// 场景: completion 包含用户定义的变量/函数
TEST(FluxLanguageServerTest, CompletionIncludesUserSymbols) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    write_lsp_message(in_file, make_initialize_request(1));
    write_lsp_message(in_file, make_initialized_notification());
    write_lsp_message(
        in_file,
        make_did_open_notification("file:///user.flux", "myData = 42\nmyFunc = (x) => x * 2\n"));

    // General completion at empty line (line 2, char 0)
    write_lsp_message(in_file, make_completion_request(2, "file:///user.flux", 2, 0));
    write_lsp_message(in_file, make_shutdown_request(3));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);

    // skip init resp
    read_lsp_response(out_file);
    // skip diagnostics
    read_lsp_response(out_file);

    // completion response
    auto comp_resp = read_lsp_response(out_file);
    ASSERT_FALSE(comp_resp.empty());
    // Should contain user-defined symbols
    EXPECT_NE(comp_resp.find("\"myData\""), std::string::npos) << comp_resp;
    EXPECT_NE(comp_resp.find("\"myFunc\""), std::string::npos) << comp_resp;

    fclose(in_file);
    fclose(out_file);
}

// ============================================================
// Phase 2: Semantic navigation tests
// ============================================================

// 场景: textDocument/definition 跳转到符号定义
TEST(FluxLanguageServerTest, GoToDefinition) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    write_lsp_message(in_file, make_initialize_request(1));
    write_lsp_message(in_file, make_initialized_notification());
    // Define 'x' on line 0, use it on line 1
    write_lsp_message(in_file, make_did_open_notification("file:///def.flux", "x = 42\ny = x + 1"));

    // Request definition at the usage of 'x' on line 1, char 4
    std::string def_req =
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///def.flux"},"position":{"line":1,"character":4}}})";
    write_lsp_message(in_file, def_req);
    write_lsp_message(in_file, make_shutdown_request(3));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);

    // skip init resp
    read_lsp_response(out_file);
    // skip diagnostics
    read_lsp_response(out_file);

    // definition response
    auto def_resp = read_lsp_response(out_file);
    ASSERT_FALSE(def_resp.empty());
    expect_valid_json(def_resp);
    // Should contain the uri and a range pointing to line 0 (definition of x)
    EXPECT_NE(def_resp.find("\"uri\""), std::string::npos) << def_resp;
    EXPECT_NE(def_resp.find("\"line\":0"), std::string::npos) << def_resp;

    fclose(in_file);
    fclose(out_file);
}

// 场景: LSP character 使用 UTF-16 code units，非 ASCII 位于同一行时仍能定位符号
TEST(FluxLanguageServerTest, GoToDefinitionAfterUtf8Text) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    write_lsp_message(in_file, make_initialize_request(1));
    write_lsp_message(in_file, make_initialized_notification());
    write_lsp_message(
        in_file, make_did_open_notification("file:///utf16.flux", "foo = 1\ny = \"你\" + foo"));

    // In UTF-16 coordinates, foo starts at character 10 on the second line.
    std::string def_req =
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///utf16.flux"},"position":{"line":1,"character":10}}})";
    write_lsp_message(in_file, def_req);
    write_lsp_message(in_file, make_shutdown_request(3));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);
    read_lsp_response(out_file);
    read_lsp_response(out_file);

    auto def_resp = read_lsp_response(out_file);
    ASSERT_FALSE(def_resp.empty());
    expect_valid_json(def_resp);
    EXPECT_NE(def_resp.find("\"line\":0"), std::string::npos) << def_resp;

    fclose(in_file);
    fclose(out_file);
}

// 场景: inline lambda 参数应跳到参数本身，而不是文档开头
TEST(FluxLanguageServerTest, GoToDefinitionForInlineLambdaParameter) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    write_lsp_message(in_file, make_initialize_request(1));
    write_lsp_message(in_file, make_initialized_notification());
    write_lsp_message(
        in_file,
        make_did_open_notification("file:///lambda.flux", "filter(fn: (r) => r.active == true)"));

    std::string def_req =
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///lambda.flux"},"position":{"line":0,"character":18}}})";
    write_lsp_message(in_file, def_req);
    write_lsp_message(in_file, make_shutdown_request(3));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);
    read_lsp_response(out_file);
    read_lsp_response(out_file);

    auto def_resp = read_lsp_response(out_file);
    ASSERT_FALSE(def_resp.empty());
    expect_valid_json(def_resp);
    EXPECT_NE(def_resp.find(R"("line":0,"character":12)"), std::string::npos) << def_resp;
    EXPECT_EQ(def_resp.find(R"("line":0,"character":0)"), std::string::npos) << def_resp;

    fclose(in_file);
    fclose(out_file);
}

// 场景: textDocument/references 查找所有引用
TEST(FluxLanguageServerTest, FindReferences) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    write_lsp_message(in_file, make_initialize_request(1));
    write_lsp_message(in_file, make_initialized_notification());
    // 'val' defined line 0, used on line 1 and line 2
    write_lsp_message(
        in_file,
        make_did_open_notification("file:///ref.flux", "val = 10\na = val + 1\nb = val * 2"));

    // Request references at 'val' definition on line 0, char 0
    std::string ref_req =
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/references","params":{"textDocument":{"uri":"file:///ref.flux"},"position":{"line":0,"character":0},"context":{"includeDeclaration":true}}})";
    write_lsp_message(in_file, ref_req);
    write_lsp_message(in_file, make_shutdown_request(3));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);

    // skip init resp
    read_lsp_response(out_file);
    // skip diagnostics
    read_lsp_response(out_file);

    // references response
    auto ref_resp = read_lsp_response(out_file);
    ASSERT_FALSE(ref_resp.empty());
    // Should return an array with at least 3 locations (definition + 2 references)
    // Count occurrences of "uri" as a proxy
    size_t count = 0;
    size_t pos = 0;
    while ((pos = ref_resp.find("\"uri\"", pos)) != std::string::npos) {
        ++count;
        ++pos;
    }
    EXPECT_GE(count, 3u) << ref_resp;

    fclose(in_file);
    fclose(out_file);
}

// 场景: references 尊重 includeDeclaration=false
TEST(FluxLanguageServerTest, FindReferencesCanExcludeDeclaration) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    write_lsp_message(in_file, make_initialize_request(1));
    write_lsp_message(in_file, make_initialized_notification());
    write_lsp_message(in_file,
                      make_did_open_notification("file:///ref-nodecl.flux",
                                                 "val = 10\na = val + 1\nb = val * 2"));

    std::string ref_req =
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/references","params":{"textDocument":{"uri":"file:///ref-nodecl.flux"},"position":{"line":0,"character":0},"context":{"includeDeclaration":false}}})";
    write_lsp_message(in_file, ref_req);
    write_lsp_message(in_file, make_shutdown_request(3));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);
    read_lsp_response(out_file);
    read_lsp_response(out_file);

    auto ref_resp = read_lsp_response(out_file);
    ASSERT_FALSE(ref_resp.empty());
    EXPECT_EQ(count_occurrences(ref_resp, "\"uri\""), 2u) << ref_resp;

    fclose(in_file);
    fclose(out_file);
}

// 场景: textDocument/rename 重命名符号
TEST(FluxLanguageServerTest, RenameSymbol) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    write_lsp_message(in_file, make_initialize_request(1));
    write_lsp_message(in_file, make_initialized_notification());
    write_lsp_message(in_file,
                      make_did_open_notification("file:///ren.flux", "foo = 1\nbar = foo + 2"));

    // Rename 'foo' to 'baz' at line 0, char 0
    std::string rename_req =
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/rename","params":{"textDocument":{"uri":"file:///ren.flux"},"position":{"line":0,"character":0},"newName":"baz"}})";
    write_lsp_message(in_file, rename_req);
    write_lsp_message(in_file, make_shutdown_request(3));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);

    // skip init resp
    read_lsp_response(out_file);
    // skip diagnostics
    read_lsp_response(out_file);

    // rename response (WorkspaceEdit)
    auto rename_resp = read_lsp_response(out_file);
    ASSERT_FALSE(rename_resp.empty());
    // Should contain "changes" with "newText":"baz"
    EXPECT_NE(rename_resp.find("\"changes\""), std::string::npos) << rename_resp;
    EXPECT_NE(rename_resp.find("\"baz\""), std::string::npos) << rename_resp;

    fclose(in_file);
    fclose(out_file);
}

// 场景: 同名函数参数按作用域解析，rename 不应改到另一个函数的参数
TEST(FluxLanguageServerTest, RenameSameNameParameterDoesNotCrossFunctionScope) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    write_lsp_message(in_file, make_initialize_request(1));
    write_lsp_message(in_file, make_initialized_notification());
    write_lsp_message(
        in_file,
        make_did_open_notification("file:///scope.flux", "f = (x) => x + 1\ng = (x) => x + 2"));

    std::string rename_req =
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/rename","params":{"textDocument":{"uri":"file:///scope.flux"},"position":{"line":1,"character":5},"newName":"y"}})";
    write_lsp_message(in_file, rename_req);
    write_lsp_message(in_file, make_shutdown_request(3));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);
    read_lsp_response(out_file);
    read_lsp_response(out_file);

    auto rename_resp = read_lsp_response(out_file);
    ASSERT_FALSE(rename_resp.empty());
    EXPECT_EQ(count_occurrences(rename_resp, R"("newText":"y")"), 2u) << rename_resp;
    EXPECT_NE(rename_resp.find(R"("line":1)"), std::string::npos) << rename_resp;
    EXPECT_EQ(rename_resp.find(R"("line":0)"), std::string::npos) << rename_resp;

    fclose(in_file);
    fclose(out_file);
}

// 场景: textDocument/signatureHelp 显示函数签名
TEST(FluxLanguageServerTest, SignatureHelp) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    write_lsp_message(in_file, make_initialize_request(1));
    write_lsp_message(in_file, make_initialized_notification());
    // User-defined function with parameters
    write_lsp_message(in_file,
                      make_did_open_notification(
                          "file:///sig.flux", "add = (a, b) => a + b\nresult = add(a: 1, b: 2)"));

    // Request signatureHelp inside the call: add(a: 1, | ) - line 1, char 18 (after first comma)
    std::string sig_req =
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/signatureHelp","params":{"textDocument":{"uri":"file:///sig.flux"},"position":{"line":1,"character":18}}})";
    write_lsp_message(in_file, sig_req);
    write_lsp_message(in_file, make_shutdown_request(3));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);

    // skip init resp
    read_lsp_response(out_file);
    // skip diagnostics
    read_lsp_response(out_file);

    // signatureHelp response
    auto sig_resp = read_lsp_response(out_file);
    ASSERT_FALSE(sig_resp.empty());
    // Should contain signatures with parameters
    EXPECT_NE(sig_resp.find("\"signatures\""), std::string::npos) << sig_resp;
    EXPECT_NE(sig_resp.find("\"parameters\""), std::string::npos) << sig_resp;

    fclose(in_file);
    fclose(out_file);
}

TEST(FluxLanguageServerTest, SignatureHelpUsesBuiltinMetadataLabel) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    write_lsp_message(in_file, make_initialize_request(1));
    write_lsp_message(in_file, make_initialized_notification());
    write_lsp_message(
        in_file,
        make_did_open_notification("file:///builtin-sig.flux", "data = range(start: -1h)\n"));
    write_lsp_message(in_file, make_completion_request(2, "file:///builtin-sig.flux", 0, 14));
    write_lsp_message(
        in_file,
        R"({"jsonrpc":"2.0","id":3,"method":"textDocument/signatureHelp","params":{"textDocument":{"uri":"file:///builtin-sig.flux"},"position":{"line":0,"character":14}}})");
    write_lsp_message(in_file, make_shutdown_request(4));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);
    read_lsp_response(out_file);
    read_lsp_response(out_file);
    read_lsp_response(out_file);

    auto sig_resp = read_lsp_response(out_file);
    ASSERT_FALSE(sig_resp.empty());
    expect_valid_json(sig_resp);
    EXPECT_NE(sig_resp.find("range(<-tables: stream[A], start: time|duration"), std::string::npos)
        << sig_resp;
    EXPECT_NE(sig_resp.find("=> stream[A]"), std::string::npos) << sig_resp;
    EXPECT_EQ(sig_resp.find("stream[dynamic]"), std::string::npos) << sig_resp;

    fclose(in_file);
    fclose(out_file);
}

TEST(FluxLanguageServerTest, HoverPrefersBuiltinSignatureOverExpressionType) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    write_lsp_message(in_file, make_initialize_request(1));
    write_lsp_message(in_file, make_initialized_notification());
    write_lsp_message(in_file,
                      make_did_open_notification("file:///hover.flux",
                                                 "data = range(start: -1h)\n"
                                                 "    |> yield(name: \"out\")\n"));
    write_lsp_message(in_file, make_hover_request(2, "file:///hover.flux", 0, 8));
    write_lsp_message(in_file, make_shutdown_request(3));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);
    read_lsp_response(out_file);
    read_lsp_response(out_file);

    auto hover_resp = read_lsp_response(out_file);
    ASSERT_FALSE(hover_resp.empty());
    expect_valid_json(hover_resp);
    EXPECT_NE(hover_resp.find("range(<-tables: stream[A], start: time|duration"), std::string::npos)
        << hover_resp;
    EXPECT_EQ(hover_resp.find("stream[dynamic]"), std::string::npos) << hover_resp;

    fclose(in_file);
    fclose(out_file);
}

TEST(FluxLanguageServerTest, HoverPrefersPackageFunctionSignatureOverExpressionType) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    write_lsp_message(in_file, make_initialize_request(1));
    write_lsp_message(in_file, make_initialized_notification());
    write_lsp_message(
        in_file,
        make_did_open_notification("file:///package-hover.flux",
                                   "import \"mysql\"\n"
                                   "data = mysql.from(host: \"127.0.0.1\", table: \"cpu\")\n"));
    write_lsp_message(in_file, make_hover_request(2, "file:///package-hover.flux", 1, 14));
    write_lsp_message(in_file, make_shutdown_request(3));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);
    read_lsp_response(out_file);
    read_lsp_response(out_file);

    auto hover_resp = read_lsp_response(out_file);
    ASSERT_FALSE(hover_resp.empty());
    expect_valid_json(hover_resp);
    EXPECT_NE(hover_resp.find("mysql.from("), std::string::npos) << hover_resp;
    EXPECT_NE(hover_resp.find("=> stream[A]"), std::string::npos) << hover_resp;
    EXPECT_EQ(hover_resp.find("stream[{}]"), std::string::npos) << hover_resp;

    fclose(in_file);
    fclose(out_file);
}

TEST(FluxLanguageServerTest, SignatureHelpUsesPackageFunctionMetadataLabel) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    write_lsp_message(in_file, make_initialize_request(1));
    write_lsp_message(in_file, make_initialized_notification());
    write_lsp_message(
        in_file,
        make_did_open_notification("file:///package-sig.flux",
                                   "import \"mysql\"\n"
                                   "data = mysql.from(host: \"127.0.0.1\", table: \"cpu\")\n"));
    write_lsp_message(
        in_file,
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/signatureHelp","params":{"textDocument":{"uri":"file:///package-sig.flux"},"position":{"line":1,"character":18}}})");
    write_lsp_message(in_file, make_shutdown_request(3));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);
    read_lsp_response(out_file);
    read_lsp_response(out_file);

    auto sig_resp = read_lsp_response(out_file);
    ASSERT_FALSE(sig_resp.empty());
    expect_valid_json(sig_resp);
    EXPECT_NE(sig_resp.find("mysql.from("), std::string::npos) << sig_resp;
    EXPECT_NE(sig_resp.find("=> stream[A]"), std::string::npos) << sig_resp;
    EXPECT_EQ(sig_resp.find("stream[{}]"), std::string::npos) << sig_resp;

    fclose(in_file);
    fclose(out_file);
}

TEST(FluxLanguageServerTest, HoverShowsContextualRowFieldType) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    write_lsp_message(in_file, make_initialize_request(1));
    write_lsp_message(in_file, make_initialized_notification());
    write_lsp_message(in_file,
                      make_did_open_notification("file:///row-hover.flux",
                                                 "import \"array\"\n"
                                                 "array.from(rows: [{_value: 1, host: \"a\"}])\n"
                                                 "    |> map(fn: (r) => ({host: r.host}))\n"));
    write_lsp_message(in_file, make_hover_request(2, "file:///row-hover.flux", 2, 35));
    write_lsp_message(in_file, make_shutdown_request(3));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);
    read_lsp_response(out_file);
    read_lsp_response(out_file);

    auto hover_resp = read_lsp_response(out_file);
    ASSERT_FALSE(hover_resp.empty());
    expect_valid_json(hover_resp);
    EXPECT_NE(hover_resp.find("`string`"), std::string::npos) << hover_resp;
    EXPECT_EQ(hover_resp.find("`dynamic`"), std::string::npos) << hover_resp;

    fclose(in_file);
    fclose(out_file);
}

// 场景: textDocument/documentHighlight 高亮符号所有出现位置
TEST(FluxLanguageServerTest, DocumentHighlight) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    write_lsp_message(in_file, make_initialize_request(1));
    write_lsp_message(in_file, make_initialized_notification());
    write_lsp_message(in_file, make_did_open_notification("file:///hl.flux", "n = 5\nm = n * n"));

    // Request documentHighlight at 'n' on line 1, char 4
    std::string hl_req =
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/documentHighlight","params":{"textDocument":{"uri":"file:///hl.flux"},"position":{"line":1,"character":4}}})";
    write_lsp_message(in_file, hl_req);
    write_lsp_message(in_file, make_shutdown_request(3));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);

    // skip init resp
    read_lsp_response(out_file);
    // skip diagnostics
    read_lsp_response(out_file);

    // documentHighlight response
    auto hl_resp = read_lsp_response(out_file);
    ASSERT_FALSE(hl_resp.empty());
    // Should return an array with highlight entries containing "kind"
    EXPECT_NE(hl_resp.find("\"kind\""), std::string::npos) << hl_resp;
    // Should have at least the definition (kind=3) and references (kind=2)
    EXPECT_NE(hl_resp.find("\"kind\":3"), std::string::npos) << hl_resp;
    EXPECT_NE(hl_resp.find("\"kind\":2"), std::string::npos) << hl_resp;

    fclose(in_file);
    fclose(out_file);
}

// ============================================================
// Phase 3: Advanced feature tests
// ============================================================

// 场景: textDocument/semanticTokens/full 返回语义着色数据
TEST(FluxLanguageServerTest, SemanticTokensFull) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    write_lsp_message(in_file, make_initialize_request(1));
    write_lsp_message(in_file, make_initialized_notification());
    write_lsp_message(
        in_file, make_did_open_notification("file:///st.flux", "x = 1\nadd = (a, b) => a + b"));

    // Request semantic tokens
    std::string st_req =
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/semanticTokens/full","params":{"textDocument":{"uri":"file:///st.flux"}}})";
    write_lsp_message(in_file, st_req);
    write_lsp_message(in_file, make_shutdown_request(3));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);

    // skip init resp
    read_lsp_response(out_file);
    // skip diagnostics
    read_lsp_response(out_file);

    // semantic tokens response
    auto st_resp = read_lsp_response(out_file);
    ASSERT_FALSE(st_resp.empty());
    // Should contain "data" array with integer values
    EXPECT_NE(st_resp.find("\"data\""), std::string::npos) << st_resp;
    // Should NOT be empty (we have definitions and references)
    EXPECT_EQ(st_resp.find("\"data\":[]"), std::string::npos) << st_resp;

    fclose(in_file);
    fclose(out_file);
}

// 场景: import 的 namespace token 应落在包名上，而不是覆盖 import 关键字
TEST(FluxLanguageServerTest, SemanticTokensImportUsesPackageNameRange) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    write_lsp_message(in_file, make_initialize_request(1));
    write_lsp_message(in_file, make_initialized_notification());
    write_lsp_message(
        in_file,
        make_did_open_notification("file:///import_tokens.flux", "import \"mysql\"\nmysql.from()"));

    std::string st_req =
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/semanticTokens/full","params":{"textDocument":{"uri":"file:///import_tokens.flux"}}})";
    write_lsp_message(in_file, st_req);
    write_lsp_message(in_file, make_shutdown_request(3));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);
    read_lsp_response(out_file);
    read_lsp_response(out_file);

    auto st_resp = read_lsp_response(out_file);
    ASSERT_FALSE(st_resp.empty());
    expect_valid_json(st_resp);
    // First namespace token: line 0, character 8, length 5, type namespace(9).
    EXPECT_NE(st_resp.find(R"("data":[0,8,5,9,)"), std::string::npos) << st_resp;
    EXPECT_EQ(st_resp.find(R"("data":[0,0,5,9,)"), std::string::npos) << st_resp;

    fclose(in_file);
    fclose(out_file);
}

// 场景: textDocument/codeAction 返回代码操作
TEST(FluxLanguageServerTest, CodeAction) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    write_lsp_message(in_file, make_initialize_request(1));
    write_lsp_message(in_file, make_initialized_notification());
    // Use a known package name 'math' without importing it — should trigger "add import" action
    write_lsp_message(in_file, make_did_open_notification("file:///ca.flux", "x = math"));

    // Request code actions for line 0
    std::string ca_req =
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"file:///ca.flux"},"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":8}},"context":{"diagnostics":[]}}})";
    write_lsp_message(in_file, ca_req);
    write_lsp_message(in_file, make_shutdown_request(3));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);

    // skip init resp
    read_lsp_response(out_file);
    // skip diagnostics
    read_lsp_response(out_file);

    // code action response
    auto ca_resp = read_lsp_response(out_file);
    ASSERT_FALSE(ca_resp.empty());
    // Should contain an "Add import" code action for 'math'
    EXPECT_NE(ca_resp.find("\"title\""), std::string::npos) << ca_resp;
    EXPECT_NE(ca_resp.find("math"), std::string::npos) << ca_resp;

    fclose(in_file);
    fclose(out_file);
}

// 场景: textDocument/selectionRange 返回选择范围
TEST(FluxLanguageServerTest, SelectionRange) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    write_lsp_message(in_file, make_initialize_request(1));
    write_lsp_message(in_file, make_initialized_notification());
    write_lsp_message(in_file, make_did_open_notification("file:///sr.flux", "x = 42\ny = x + 1"));

    // Request selection range at position (0, 0) — on 'x'
    std::string sr_req =
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/selectionRange","params":{"textDocument":{"uri":"file:///sr.flux"},"positions":[{"line":0,"character":0}]}})";
    write_lsp_message(in_file, sr_req);
    write_lsp_message(in_file, make_shutdown_request(3));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);

    // skip init resp
    read_lsp_response(out_file);
    // skip diagnostics
    read_lsp_response(out_file);

    // selection range response
    auto sr_resp = read_lsp_response(out_file);
    ASSERT_FALSE(sr_resp.empty());
    // Should contain a range with start/end
    EXPECT_NE(sr_resp.find("\"range\""), std::string::npos) << sr_resp;
    // Should have a parent (the enclosing statement)
    EXPECT_NE(sr_resp.find("\"parent\""), std::string::npos) << sr_resp;

    fclose(in_file);
    fclose(out_file);
}

// 场景: capabilities 包含 Phase 3 功能声明
TEST(FluxLanguageServerTest, Phase3Capabilities) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    write_lsp_message(in_file, make_initialize_request(1));
    write_lsp_message(in_file, make_initialized_notification());
    write_lsp_message(in_file, make_shutdown_request(2));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);

    auto resp = read_lsp_response(out_file);
    ASSERT_FALSE(resp.empty());
    // Phase 3 capabilities
    EXPECT_NE(resp.find("semanticTokensProvider"), std::string::npos) << resp;
    EXPECT_NE(resp.find("codeActionProvider"), std::string::npos) << resp;
    EXPECT_NE(resp.find("inlayHintProvider"), std::string::npos) << resp;
    EXPECT_NE(resp.find("selectionRangeProvider"), std::string::npos) << resp;
    // Version bump
    EXPECT_NE(resp.find("0.4.0"), std::string::npos) << resp;

    fclose(in_file);
    fclose(out_file);
}

// 场景: 发送 shutdown 后 server 返回 null result，再发 exit 后 server 退出
TEST(FluxLanguageServerTest, ShutdownAndExit) {
    FILE* in_file = tmpfile();
    FILE* out_file = tmpfile();
    ASSERT_NE(in_file, nullptr);
    ASSERT_NE(out_file, nullptr);

    write_lsp_message(in_file, make_initialize_request(1));
    write_lsp_message(in_file, make_initialized_notification());
    write_lsp_message(in_file, make_shutdown_request(2));
    write_lsp_message(in_file, make_exit_notification());
    rewind(in_file);

    StdioTransport transport(in_file, out_file);
    FluxLanguageServer server(std::move(transport));
    server.run();

    rewind(out_file);

    // 跳过 initialize response
    auto init_resp = read_lsp_response(out_file);
    ASSERT_FALSE(init_resp.empty());

    // 读取 shutdown response
    auto shutdown_resp = read_lsp_response(out_file);
    ASSERT_FALSE(shutdown_resp.empty());
    // LSP 规范: shutdown 的 result 必须为 null
    EXPECT_NE(shutdown_resp.find("null"), std::string::npos);

    // server.run() 正常返回即说明 exit 后 server 已退出
    // 若 run() 未退出，测试会卡住 (超时失败)

    fclose(in_file);
    fclose(out_file);
}
