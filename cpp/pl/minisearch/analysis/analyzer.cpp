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

#include "cpp/pl/minisearch/analysis/analyzer.h"

#include <jieba.h>
#include <mutex>

namespace pl::minisearch::analysis {

namespace {

// Jieba-CPP 词典为进程级单例：多 collection 共享，首次访问时加载。
Jieba::Jieba& SharedJieba() {
    static Jieba::Jieba* instance = nullptr;
    static std::once_flag once;
    std::call_once(once, [] { instance = new Jieba::Jieba(); });
    return *instance;
}

} // namespace

std::vector<Token> RawAnalyzer::Analyze(std::string_view text) const {
    std::vector<Token> tokens;
    uint32_t pos = 0;
    size_t start = 0;
    while (start < text.size()) {
        while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
            ++start;
        }
        if (start >= text.size()) {
            break;
        }
        size_t end = start;
        while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end]))) {
            ++end;
        }
        tokens.push_back({std::string(text.substr(start, end - start)),
                          pos++,
                          static_cast<uint32_t>(start),
                          static_cast<uint32_t>(end)});
        start = end;
    }
    return tokens;
}

JiebaAnalyzer::JiebaAnalyzer() = default;

std::vector<Token> JiebaAnalyzer::Analyze(std::string_view text) const {
    std::vector<Token> tokens;
    // cut() 返回指向输入 buffer 的 string_view，偏移由指针差得出。
    const std::vector<std::string_view> words = SharedJieba().cut(text);
    tokens.reserve(words.size());
    const char* base = text.data();
    uint32_t pos = 0;
    for (const std::string_view word : words) {
        if (word.empty()) {
            continue;
        }
        const size_t begin = static_cast<size_t>(word.data() - base);
        tokens.push_back({std::string(word),
                          pos++,
                          static_cast<uint32_t>(begin),
                          static_cast<uint32_t>(begin + word.size())});
    }
    return tokens;
}

bool IsKnownAnalyzer(const std::string& name) {
    return name.empty() || name == "raw" || name == "cjk_jieba";
}

std::unique_ptr<Analyzer> CreateAnalyzer(const std::string& name) {
    if (name == "raw") {
        return std::make_unique<RawAnalyzer>();
    }
    if (name.empty() || name == "cjk_jieba") {
        return std::make_unique<JiebaAnalyzer>();
    }
    return nullptr;
}

} // namespace pl::minisearch::analysis
