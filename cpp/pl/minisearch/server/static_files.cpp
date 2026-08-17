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
// Created: 2026/08/17

#include "cpp/pl/minisearch/server/static_files.h"

#include <cctype>
#include <fstream>
#include <unordered_map>

namespace pl::minisearch::server {

namespace {

const std::unordered_map<std::string, std::string>& content_types() {
    static const auto* types = new std::unordered_map<std::string, std::string>{
        {".html", "text/html; charset=utf-8"},
        {".js", "application/javascript; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},
        {".json", "application/json; charset=utf-8"},
        {".svg", "image/svg+xml"},
        {".png", "image/png"},
        {".ico", "image/x-icon"},
        {".woff2", "font/woff2"},
        {".txt", "text/plain; charset=utf-8"},
        {".md", "text/markdown; charset=utf-8"},
    };
    return *types;
}

std::string content_type_for(const std::string& path) {
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos) {
        return "application/octet-stream";
    }
    const auto it = content_types().find(path.substr(dot));
    return it == content_types().end() ? "application/octet-stream" : it->second;
}

// 仅允许 [A-Za-z0-9._/-]，拒绝 ".."、控制字符与反斜杠，防目录穿越。
bool is_safe_path(const std::string& path) {
    if (path.find("..") != std::string::npos || path.find('\\') != std::string::npos) {
        return false;
    }
    for (const char c : path) {
        const auto uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) == 0 && c != '.' && c != '_' && c != '/' && c != '-') {
            return false;
        }
    }
    return true;
}

} // namespace

bool ServeStaticFile(const std::string& web_dir, const std::string& path, brpc::Controller* cntl) {
    std::string rel = path;
    while (!rel.empty() && rel.front() == '/') {
        rel.erase(rel.begin());
    }
    if (rel.empty() || rel.back() == '/') {
        rel += "index.html";
    }
    if (!is_safe_path(rel)) {
        return false;
    }
    const std::string full = web_dir + "/" + rel;
    std::ifstream in(full, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    cntl->http_response().set_status_code(200);
    cntl->http_response().set_content_type(content_type_for(rel));
    cntl->http_response().SetHeader("Cache-Control", "no-cache");
    cntl->response_attachment().append(content);
    return true;
}

} // namespace pl::minisearch::server
