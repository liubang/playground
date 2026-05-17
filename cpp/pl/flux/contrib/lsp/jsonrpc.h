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
// Created: 2026/05/18 00:34

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace pl::flux::lsp {

// JSON-RPC 2.0 error codes
enum class ErrorCode : int {
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,

    // LSP-specific error codes
    ServerNotInitialized = -32002,
    RequestCancelled = -32800,
    ContentModified = -32801,
};

// A JSON-RPC request/notification message (parsed).
// For notifications, id is std::nullopt.
struct JsonRpcMessage {
    std::optional<std::variant<int64_t, std::string>> id;
    std::string method;
    std::string params; // Raw JSON string of params (to be parsed on demand)
};

// A JSON-RPC error object
struct JsonRpcError {
    ErrorCode code;
    std::string message;
};

// Build a JSON-RPC response string (success)
std::string make_response(const std::variant<int64_t, std::string>& id,
                          const std::string& result_json);

// Build a JSON-RPC response string (error)
std::string make_error_response(const std::variant<int64_t, std::string>& id,
                                const JsonRpcError& error);

// Build a JSON-RPC notification string (server -> client)
std::string make_notification(const std::string& method, const std::string& params_json);

// Parse a raw JSON string into a JsonRpcMessage.
// Returns std::nullopt if the JSON is malformed or not a valid JSON-RPC message.
std::optional<JsonRpcMessage> parse_message(const std::string& json);

} // namespace pl::flux::lsp
