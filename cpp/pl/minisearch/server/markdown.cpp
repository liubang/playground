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

#include "cpp/pl/minisearch/server/markdown.h"

#include <cctype>
#include <utility>

namespace pl::minisearch::server {

namespace {

std::string trim(const std::string& s) {
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin])) != 0) {
        ++begin;
    }
    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) {
        --end;
    }
    return s.substr(begin, end - begin);
}

// ATX 标题：^#{1,6}\s+text。返回 (level, text)；非标题返回 (0, "")。
std::pair<int, std::string> parse_heading(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && line[i] == '#') {
        ++i;
    }
    if (i == 0 || i > 6 || i >= line.size() || (line[i] != ' ' && line[i] != '\t')) {
        return {0, ""};
    }
    std::string text = trim(line.substr(i));
    // 去掉结尾的闭合 # 序列（## title ##）
    size_t end = text.size();
    while (end > 0 && text[end - 1] == '#') {
        --end;
    }
    return {static_cast<int>(i), trim(text.substr(0, end))};
}

bool is_fence(const std::string& line) {
    const std::string t = trim(line);
    return t.rfind("```", 0) == 0 || t.rfind("~~~", 0) == 0;
}

// 按空行拆块；fence 代码块作为原子单位（不跨 fence 拆分）。
std::vector<std::string> split_blocks(const std::string& text) {
    std::vector<std::string> blocks;
    std::string current;
    bool in_fence = false;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        const std::string line =
            nl == std::string::npos ? text.substr(pos) : text.substr(pos, nl - pos);
        const bool blank = trim(line).empty();
        if (is_fence(line)) {
            in_fence = !in_fence;
        }
        if (blank && !in_fence) {
            if (!trim(current).empty()) {
                blocks.push_back(trim(current));
            }
            current.clear();
        } else {
            if (!current.empty()) {
                current.push_back('\n');
            }
            current += line;
        }
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }
    if (!trim(current).empty()) {
        blocks.push_back(trim(current));
    }
    return blocks;
}

// 单个超长块按 max_chars 硬切（保留 overlap）。
// 起止位置都回退/对齐到 UTF-8 字符边界，避免产出非法 UTF-8 的 chunk。
void hard_split(const std::string& block, const ChunkOptions& opts, std::vector<std::string>* out) {
    const size_t step =
        opts.max_chars > opts.overlap_chars ? opts.max_chars - opts.overlap_chars : opts.max_chars;
    size_t pos = 0;
    while (pos < block.size()) {
        // 起点对齐：跳过上一 chunk 尾部截断残留的 UTF-8 continuation bytes
        while (pos < block.size() && (static_cast<unsigned char>(block[pos]) & 0xC0) == 0x80) {
            ++pos;
        }
        if (pos >= block.size()) {
            break;
        }
        size_t len = opts.max_chars;
        if (pos + len < block.size()) {
            // 终点回退到字符边界
            while (len > 0 && (static_cast<unsigned char>(block[pos + len]) & 0xC0) == 0x80) {
                --len;
            }
        }
        out->push_back(block.substr(pos, len));
        pos += step;
    }
}

// 块列表贪心合并成 <= max_chars 的 chunk；单块超限则硬切。
std::vector<std::string> merge_blocks(const std::vector<std::string>& blocks,
                                      const ChunkOptions& opts) {
    std::vector<std::string> chunks;
    std::string current;
    for (const auto& block : blocks) {
        if (block.size() > opts.max_chars) {
            if (!current.empty()) {
                chunks.push_back(current);
                current.clear();
            }
            hard_split(block, opts, &chunks);
            continue;
        }
        const size_t joined = current.empty() ? block.size() : current.size() + 1 + block.size();
        if (joined > opts.max_chars) {
            chunks.push_back(current);
            current = block;
        } else {
            if (!current.empty()) {
                current.push_back('\n');
            }
            current += block;
        }
    }
    if (!current.empty()) {
        chunks.push_back(std::move(current));
    }
    return chunks;
}

void append_chunks(const std::string& title_path,
                   const std::string& body,
                   const ChunkOptions& opts,
                   std::vector<MarkdownChunk>* out) {
    const std::string trimmed = trim(body);
    if (trimmed.empty()) {
        return;
    }
    for (auto& text : merge_blocks(split_blocks(trimmed), opts)) {
        out->push_back(MarkdownChunk{title_path, std::move(text)});
    }
}

} // namespace

std::vector<MarkdownChunk> ChunkMarkdown(const std::string& markdown, const ChunkOptions& opts) {
    std::vector<MarkdownChunk> chunks;
    // 标题栈：levels[i] 单调递增，path[i] 为对应层级标题文本
    std::vector<int> levels;
    std::vector<std::string> titles;
    std::string body; // 当前 section 的累积正文
    bool in_fence = false;

    auto title_path = [&]() {
        std::string out;
        for (const auto& t : titles) {
            if (!out.empty()) {
                out += " > ";
            }
            out += t;
        }
        return out;
    };

    size_t pos = 0;
    while (pos <= markdown.size()) {
        const size_t nl = markdown.find('\n', pos);
        const std::string line =
            nl == std::string::npos ? markdown.substr(pos) : markdown.substr(pos, nl - pos);
        if (is_fence(line)) {
            in_fence = !in_fence;
        }
        if (!in_fence) {
            const auto [level, text] = parse_heading(line);
            if (level > 0) {
                // 新标题：flush 上一个 section，更新标题栈
                append_chunks(title_path(), body, opts, &chunks);
                body.clear();
                while (!levels.empty() && levels.back() >= level) {
                    levels.pop_back();
                    titles.pop_back();
                }
                levels.push_back(level);
                titles.push_back(text);
                if (nl == std::string::npos) {
                    break;
                }
                pos = nl + 1;
                continue;
            }
        }
        body += line;
        body.push_back('\n');
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }
    append_chunks(title_path(), body, opts, &chunks);
    return chunks;
}

std::vector<MarkdownChunk> ChunkFixed(const std::string& text, const ChunkOptions& opts) {
    std::vector<MarkdownChunk> chunks;
    for (auto& body : merge_blocks(split_blocks(trim(text)), opts)) {
        chunks.push_back(MarkdownChunk{"", std::move(body)});
    }
    return chunks;
}

} // namespace pl::minisearch::server
