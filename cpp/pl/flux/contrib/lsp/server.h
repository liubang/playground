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
#include "cpp/pl/flux/contrib/lsp/transport.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace pl::flux::lsp {

// Server-level options, configurable via CLI args and/or initializationOptions.
struct ServerOptions {
    FormatOptions format;
};

// Per-document state maintained by the server.
struct Document {
    std::string uri;
    std::string content;
    int version = 0;
};

// Flux Language Server implementation.
// Supports: initialize, textDocument/didOpen, didChange, didClose,
//           textDocument/completion, textDocument/hover,
//           textDocument/publishDiagnostics (push).
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

    // Publish diagnostics for a document.
    void publish_diagnostics(const std::string& uri);

    // Build completion items from current context.
    std::string build_completion_response(const Document& doc, int line, int character);

    // Build hover response at a position.
    std::string build_hover_response(const Document& doc, int line, int character);

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
