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
// Created: 2026/05/18 00:32

#include <charconv>
#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>

#include "cpp/pl/flux/contrib/lsp/server.h"

namespace {

void print_usage(const char* prog) {
    std::fprintf(stderr,
                 "Usage: %s [OPTIONS]\n"
                 "\n"
                 "Flux Language Server\n"
                 "\n"
                 "Options:\n"
                 "  --max-line-width=N   Maximum line width for formatting (default: 120)\n"
                 "  --indent-width=N     Number of spaces per indent level (default: 4)\n"
                 "  --use-tabs           Use tabs instead of spaces for indentation\n"
                 "  --help, -h           Show this help message\n"
                 "  --version, -v        Show version\n",
                 prog);
}

void print_version() {
    std::fprintf(stderr, "flux-ls 0.1.0\n");
}

// 安全解析正整数，失败返回 0
int parse_positive_int(std::string_view sv) {
    int value = 0;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), value);
    if (ec != std::errc{} || ptr != sv.data() + sv.size()) {
        return 0;
    }
    return value;
}

} // namespace

int main(int argc, char* argv[]) {
    pl::flux::lsp::ServerOptions opts;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--version" || arg == "-v") {
            print_version();
            return 0;
        }
        if (arg == "--use-tabs") {
            opts.format.use_tabs = true;
            continue;
        }
        if (arg.starts_with("--max-line-width=")) {
            arg.remove_prefix(17);
            int n = parse_positive_int(arg);
            if (n > 0) {
                opts.format.max_line_width = n;
            }
            continue;
        }
        if (arg.starts_with("--indent-width=")) {
            arg.remove_prefix(15);
            int n = parse_positive_int(arg);
            if (n > 0) {
                opts.format.indent_width = n;
            }
            continue;
        }

        std::fprintf(stderr, "Unknown option: %.*s\n", static_cast<int>(arg.size()), arg.data());
        print_usage(argv[0]);
        return 1;
    }

    // Ensure binary mode on stdio (important on Windows, no-op on Unix)
    std::setvbuf(stdin, nullptr, _IONBF, 0);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    pl::flux::lsp::StdioTransport transport(stdin, stdout);
    pl::flux::lsp::FluxLanguageServer server(std::move(transport), opts);
    server.run();

    return 0;
}
