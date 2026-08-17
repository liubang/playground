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

#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "cpp/pl/minisearch/server/markdown.h"

namespace pms = pl::minisearch::server;

namespace {

// 逐字节校验 UTF-8 编码合法性（不依赖 icu/iconv，够用即可）。
bool is_valid_utf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        size_t len = 0;
        if (c < 0x80) {
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
        } else {
            return false;
        }
        if (i + len > s.size()) {
            return false;
        }
        for (size_t j = 1; j < len; ++j) {
            if ((static_cast<unsigned char>(s[i + j]) & 0xC0) != 0x80) {
                return false;
            }
        }
        i += len;
    }
    return true;
}

std::string repeat(const std::string& s, size_t n) {
    std::string out;
    out.reserve(s.size() * n);
    for (size_t i = 0; i < n; ++i) {
        out += s;
    }
    return out;
}

} // namespace

TEST(MarkdownChunkTest, TitleAwareSections) {
    const std::string md =
        "# Guide\n\nIntro paragraph.\n\n## Install\n\nInstall body.\n\n### Deps\n\nDeps body.\n";
    pms::ChunkOptions opts;
    auto chunks = pms::ChunkMarkdown(md, opts);
    ASSERT_EQ(chunks.size(), 3u);
    EXPECT_EQ(chunks[0].title_path, "Guide");
    EXPECT_EQ(chunks[1].title_path, "Guide > Install");
    EXPECT_EQ(chunks[2].title_path, "Guide > Install > Deps");
    EXPECT_NE(chunks[1].text.find("Install body."), std::string::npos);
}

TEST(MarkdownChunkTest, FenceBlockIsAtomic) {
    // 代码块内的 # 注释不得被识别为标题，空行不得拆块。
    const std::string md = "# T\n\n```cpp\n// # not a heading\n\nint x;\n```\n";
    pms::ChunkOptions opts;
    auto chunks = pms::ChunkMarkdown(md, opts);
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].title_path, "T");
    EXPECT_NE(chunks[0].text.find("int x;"), std::string::npos);
}

TEST(MarkdownChunkTest, HardSplitKeepsUtf8Boundary) {
    // 中文段落（每字 3 字节）远超 max_chars，触发硬切；
    // 每个 chunk 都必须是合法 UTF-8（起止都落在字符边界上）。
    const std::string para = repeat("混合检索服务控制台", 40); // 3*8*40 = 960 bytes
    pms::ChunkOptions opts;
    opts.max_chars = 100;
    opts.overlap_chars = 20;
    auto chunks = pms::ChunkFixed(para, opts);
    ASSERT_GT(chunks.size(), 1u);
    for (const auto& c : chunks) {
        EXPECT_TRUE(is_valid_utf8(c.text)) << "chunk is not valid UTF-8";
        EXPECT_LE(c.text.size(), opts.max_chars);
    }
    // 重叠生效：chunk[1] 以 chunk[0] 的某个非空后缀开头
    bool has_overlap = false;
    for (size_t k = 1; k < chunks[0].text.size(); ++k) {
        if (chunks[1].text.compare(0, chunks[0].text.size() - k, chunks[0].text, k) == 0) {
            has_overlap = true;
            break;
        }
    }
    EXPECT_TRUE(has_overlap);
}

TEST(MarkdownChunkTest, MarkdownHardSplitKeepsUtf8Boundary) {
    const std::string md = "# 标题\n\n" + repeat("标题感知切分策略", 50) + "\n";
    pms::ChunkOptions opts;
    opts.max_chars = 120;
    opts.overlap_chars = 30;
    auto chunks = pms::ChunkMarkdown(md, opts);
    ASSERT_GT(chunks.size(), 1u);
    for (const auto& c : chunks) {
        EXPECT_EQ(c.title_path, "标题");
        EXPECT_TRUE(is_valid_utf8(c.text)) << "chunk is not valid UTF-8";
    }
}

TEST(MarkdownChunkTest, EmptyContentProducesNoChunks) {
    pms::ChunkOptions opts;
    EXPECT_TRUE(pms::ChunkMarkdown("", opts).empty());
    EXPECT_TRUE(pms::ChunkFixed("   \n\n  ", opts).empty());
}
