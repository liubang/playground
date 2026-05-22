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

#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <string>
#include <string_view>

namespace pl::flux::lsp {

namespace {

bool iequals(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
            std::tolower(static_cast<unsigned char>(rhs[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

StdioTransport::StdioTransport(FILE* in, FILE* out) : in_(in), out_(out) {}

std::optional<std::string> StdioTransport::read_message() {
    if (!in_) {
        return std::nullopt;
    }

    // Read headers until we find Content-Length
    int content_length = -1;
    std::array<char, 256> line{};

    while (true) {
        if (!std::fgets(line.data(), static_cast<int>(line.size()), in_)) {
            return std::nullopt; // EOF
        }

        // Empty line (just \r\n) signals end of headers
        if (std::strcmp(line.data(), "\r\n") == 0 || std::strcmp(line.data(), "\n") == 0) {
            break;
        }

        std::string_view header(line.data(), std::strlen(line.data()));
        const auto colon = header.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }

        if (iequals(header.substr(0, colon), "Content-Length")) {
            const char* value_start = line.data() + colon + 1;
            const char* value_end = line.data() + header.size();
            while (value_start < value_end &&
                   std::isspace(static_cast<unsigned char>(*value_start))) {
                ++value_start;
            }
            // Trim trailing whitespace / \r\n
            while (value_end > value_start &&
                   std::isspace(static_cast<unsigned char>(*(value_end - 1)))) {
                --value_end;
            }
            auto result = std::from_chars(value_start, value_end, content_length);
            if (result.ec != std::errc() || result.ptr != value_end) {
                return std::nullopt;
            }
        }
    }

    if (content_length < 0) {
        return std::nullopt;
    }

    // Read body
    std::string body(static_cast<size_t>(content_length), '\0');
    auto bytes_read = std::fread(body.data(), 1, static_cast<size_t>(content_length), in_);
    if (bytes_read != static_cast<size_t>(content_length)) {
        return std::nullopt;
    }

    return body;
}

void StdioTransport::write_message(const std::string& json) {
    if (!out_) {
        return;
    }
    std::fprintf(out_, "Content-Length: %zu\r\n\r\n", json.size());
    std::fwrite(json.data(), 1, json.size(), out_);
    std::fflush(out_);
}

} // namespace pl::flux::lsp
