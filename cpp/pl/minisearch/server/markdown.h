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

#include <cstddef>
#include <string>
#include <vector>

namespace pl::minisearch::server {

// Chunk 大小按字节计（近似值：英文 ~4 字符/token，UTF-8 中文 3 字节/字）；
// 默认 1000 字节的目标长度在 bge-m3 的 512 token 上限内留有充分余量。
struct ChunkOptions {
    size_t max_chars = 1000;    // 目标 chunk 字节数
    size_t overlap_chars = 100; // 超长二级切分时相邻 chunk 的重叠字节数
};

struct MarkdownChunk {
    std::string title_path; // 标题路径，如 "指南 > 安装 > 依赖"；无标题时为空
    std::string text;       // chunk 正文
};

// 标题感知切分（等价 LangChain MarkdownHeaderTextSplitter + 递归兜底）：
// 按 ATX 标题（# ~ ######）分节并记录标题路径；超长 section 按空行段落
// 贪心合并二级切分（fence 代码块为原子单位，单个超长块硬切并保留 overlap）。
std::vector<MarkdownChunk> ChunkMarkdown(const std::string& markdown, const ChunkOptions& opts);

// 定长切分：按空行段落贪心合并到 max_chars，超长段落按 max_chars 硬切，
// 相邻 chunk 重叠 overlap_chars。title_path 恒为空。
std::vector<MarkdownChunk> ChunkFixed(const std::string& text, const ChunkOptions& opts);

} // namespace pl::minisearch::server
