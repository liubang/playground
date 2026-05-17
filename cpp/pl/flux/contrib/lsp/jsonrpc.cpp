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

#include "cpp/pl/flux/contrib/lsp/jsonrpc.h"

#include "simdjson.h"
#include <sstream>

namespace pl::flux::lsp {

namespace {

std::string id_to_json(const std::variant<int64_t, std::string>& id) {
    if (auto* n = std::get_if<int64_t>(&id)) {
        return std::to_string(*n);
    }
    // String id: escape for JSON
    const auto& s = std::get<std::string>(id);
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += c;
        }
    }
    out += '"';
    return out;
}

std::string escape_json_string(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += c;
        }
    }
    out += '"';
    return out;
}

} // namespace

std::string make_response(const std::variant<int64_t, std::string>& id,
                          const std::string& result_json) {
    std::ostringstream os;
    os << R"({"jsonrpc":"2.0","id":)" << id_to_json(id) << R"(,"result":)" << result_json << "}";
    return os.str();
}

std::string make_error_response(const std::variant<int64_t, std::string>& id,
                                const JsonRpcError& error) {
    std::ostringstream os;
    os << R"({"jsonrpc":"2.0","id":)" << id_to_json(id) << R"(,"error":{"code":)"
       << static_cast<int>(error.code) << R"(,"message":)" << escape_json_string(error.message)
       << "}}";
    return os.str();
}

std::string make_notification(const std::string& method, const std::string& params_json) {
    std::ostringstream os;
    os << R"({"jsonrpc":"2.0","method":)" << escape_json_string(method) << R"(,"params":)"
       << params_json << "}";
    return os.str();
}

std::optional<JsonRpcMessage> parse_message(const std::string& json) {
    simdjson::dom::parser parser;
    auto doc = parser.parse(json);
    if (doc.error()) {
        return std::nullopt;
    }

    JsonRpcMessage msg;

    // Parse method (required)
    auto method_val = doc["method"];
    if (method_val.error()) {
        return std::nullopt;
    }
    auto method_str = method_val.get_c_str();
    if (method_str.error()) {
        return std::nullopt;
    }
    msg.method = std::string(method_str.value());

    // Parse id (optional - notifications don't have it)
    auto id_val = doc["id"];
    if (!id_val.error()) {
        auto id_int = id_val.get_int64();
        if (!id_int.error()) {
            msg.id = id_int.value();
        } else {
            auto id_str = id_val.get_c_str();
            if (!id_str.error()) {
                msg.id = std::string(id_str.value());
            }
            // else: null id or unexpected type, treat as notification
        }
    }

    // Parse params (optional) - store as raw JSON string
    auto params_val = doc["params"];
    if (!params_val.error()) {
        msg.params = simdjson::minify(params_val.value());
    } else {
        msg.params = "{}";
    }

    return msg;
}

} // namespace pl::flux::lsp
