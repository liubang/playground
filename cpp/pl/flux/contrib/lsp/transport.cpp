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

#include "cpp/pl/flux/contrib/lsp/transport.h"

#include <charconv>
#include <cstring>
#include <string>

namespace pl::flux::lsp {

StdioTransport::StdioTransport(FILE* in, FILE* out) : in_(in), out_(out) {}

std::optional<std::string> StdioTransport::read_message() {
    // Read headers until we find Content-Length
    int content_length = -1;
    char line[256];

    while (true) {
        if (!std::fgets(line, sizeof(line), in_)) {
            return std::nullopt; // EOF
        }

        // Empty line (just \r\n) signals end of headers
        if (std::strcmp(line, "\r\n") == 0 || std::strcmp(line, "\n") == 0) {
            break;
        }

        // Parse Content-Length header
        constexpr char kContentLength[] = "Content-Length: ";
        if (std::strncmp(line, kContentLength, sizeof(kContentLength) - 1) == 0) {
            const char* value_start = line + sizeof(kContentLength) - 1;
            const char* value_end = value_start + std::strlen(value_start);
            // Trim trailing whitespace / \r\n
            while (value_end > value_start &&
                   (*(value_end - 1) == '\r' || *(value_end - 1) == '\n')) {
                --value_end;
            }
            auto result = std::from_chars(value_start, value_end, content_length);
            if (result.ec != std::errc()) {
                return std::nullopt;
            }
        }
    }

    if (content_length <= 0) {
        return std::nullopt;
    }

    // Read body
    std::string body(static_cast<size_t>(content_length), '\0');
    size_t bytes_read = std::fread(body.data(), 1, static_cast<size_t>(content_length), in_);
    if (bytes_read != static_cast<size_t>(content_length)) {
        return std::nullopt;
    }

    return body;
}

void StdioTransport::write_message(const std::string& json) {
    std::fprintf(out_, "Content-Length: %zu\r\n\r\n%s", json.size(), json.c_str());
    std::fflush(out_);
}

} // namespace pl::flux::lsp
