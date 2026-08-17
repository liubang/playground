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

#pragma once

#include <brpc/controller.h>
#include <string>

namespace pl::minisearch::server {

// 从 web_dir serve 单个静态文件（console 前端）。path 是 URL 前缀之后的
// 相对路径（如 "index.html"、"js/app.js"）；空路径或以 '/' 结尾解析为
// index.html。路径含 ".."/反斜杠/控制字符或文件不存在时返回 false，
// 由调用方发 404。每次请求从磁盘读取（开发期改前端文件刷新即生效），
// 响应带 Cache-Control: no-cache。
bool ServeStaticFile(const std::string& web_dir, const std::string& path, brpc::Controller* cntl);

} // namespace pl::minisearch::server
