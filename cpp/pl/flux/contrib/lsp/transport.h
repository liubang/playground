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

#pragma once

#include <cstdio>
#include <optional>
#include <string>

namespace pl::flux::lsp {

// LSP Transport layer: reads/writes LSP messages over stdio.
// The LSP wire protocol uses HTTP-style headers:
//   Content-Length: <length>\r\n
//   \r\n
//   <json body>
class StdioTransport {
public:
    explicit StdioTransport(FILE* in = stdin, FILE* out = stdout);

    // Read a single LSP message body from input.
    // Returns std::nullopt on EOF or read error.
    std::optional<std::string> read_message();

    // Write a single LSP message to output.
    void write_message(const std::string& json);

private:
    FILE* in_;
    FILE* out_;
};

} // namespace pl::flux::lsp
