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
// Created: 2026/05/18 00:27

#pragma once

#include "cpp/pl/flux/contrib/lsp/formatter.h"
#include "cpp/pl/flux/contrib/lsp/jsonrpc.h"
#include "cpp/pl/flux/contrib/lsp/symbol_table.h"
#include "cpp/pl/flux/contrib/lsp/transport.h"
#include "cpp/pl/flux/syntax/ast.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace pl::flux::lsp {

// Server-level options, configurable via CLI args and/or initializationOptions.
struct ServerOptions {
    FormatOptions format;
};

// Per-document state maintained by the server, including cached AST.
struct Document {
    std::string uri;
    std::string content;
    int version = 0;

    // Cached parse result — invalidated on content change.
    std::shared_ptr<File> ast;
    std::vector<std::string> parse_errors;
    int ast_version = -1; // version at which ast was computed

    // Cached symbol table — rebuilt when AST changes.
    SymbolTable symbols;
    int symbols_version = -1;
};

// Flux Language Server implementation.
class FluxLanguageServer {
public:
    explicit FluxLanguageServer(StdioTransport transport, ServerOptions opts = {});

    // Run the main event loop until shutdown.
    void run();

private:
    // Dispatch a parsed JSON-RPC message.
    void dispatch(const JsonRpcMessage& msg);

    // LSP lifecycle
    void handle_initialize(const JsonRpcMessage& msg);
    void handle_initialized(const JsonRpcMessage& msg);
    void handle_shutdown(const JsonRpcMessage& msg);
    void handle_exit(const JsonRpcMessage& msg);

    // Document synchronization
    void handle_did_open(const JsonRpcMessage& msg);
    void handle_did_change(const JsonRpcMessage& msg);
    void handle_did_close(const JsonRpcMessage& msg);

    // Language features
    void handle_completion(const JsonRpcMessage& msg);
    void handle_hover(const JsonRpcMessage& msg);
    void handle_formatting(const JsonRpcMessage& msg);
    void handle_document_symbol(const JsonRpcMessage& msg);
    void handle_folding_range(const JsonRpcMessage& msg);
    void handle_definition(const JsonRpcMessage& msg);
    void handle_references(const JsonRpcMessage& msg);
    void handle_rename(const JsonRpcMessage& msg);
    void handle_signature_help(const JsonRpcMessage& msg);
    void handle_document_highlight(const JsonRpcMessage& msg);
    void handle_semantic_tokens(const JsonRpcMessage& msg);
    void handle_code_action(const JsonRpcMessage& msg);
    void handle_inlay_hint(const JsonRpcMessage& msg);
    void handle_selection_range(const JsonRpcMessage& msg);

    // Ensure cached AST is up-to-date for a document.
    void ensure_ast(Document& doc);

    // Ensure cached symbol table is up-to-date for a document.
    void ensure_symbols(Document& doc);

    // Publish diagnostics for a document.
    void publish_diagnostics(const std::string& uri);

    // Build completion items from current context.
    std::string build_completion_response(Document& doc, int line, int character);

    // Build hover response at a position.
    static std::string build_hover_response(const Document& doc, int line, int character);

    // Send a response for a request.
    void reply(const JsonRpcMessage& msg, const std::string& result_json);
    void reply_error(const JsonRpcMessage& msg, ErrorCode code, const std::string& message);

    // Send a notification to the client.
    void notify(const std::string& method, const std::string& params_json);

    StdioTransport transport_;
    ServerOptions opts_;
    std::unordered_map<std::string, Document> documents_;
    bool initialized_ = false;
    bool shutdown_requested_ = false;
};

} // namespace pl::flux::lsp
