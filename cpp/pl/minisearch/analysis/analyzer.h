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

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pl::minisearch::analysis {

// 分词结果：term 及其在原文中的位置（高亮与 debug 端点使用）。
struct Token {
    std::string term;
    uint32_t pos = 0;   // token 序号（同义词扩展时多个 term 共享 pos）
    uint32_t begin = 0; // 原文字节偏移
    uint32_t end = 0;
};

// 可插拔分词器（DESIGN.md §6.1）。实现须线程安全（查询热路径共享实例）。
class Analyzer {
public:
    virtual ~Analyzer() = default;
    virtual std::vector<Token> Analyze(std::string_view text) const = 0;
};

// 空白切分（代码/标识符场景）。
class RawAnalyzer : public Analyzer {
public:
    std::vector<Token> Analyze(std::string_view text) const override;
};

// 中文混合分词：Jieba-CPP（词典 MP + HMM 新词；ASCII 字母数字串保持整词，
// ASCII 标点丢弃——由 Jieba-CPP 语义决定）。
class JiebaAnalyzer : public Analyzer {
public:
    // 词典为进程级单例（解压后 ~13MB，首次 Analyze 时加载）。
    JiebaAnalyzer();
    std::vector<Token> Analyze(std::string_view text) const override;
};

// 已知 analyzer 名称：""（= 默认 cjk_jieba）、"raw"、"cjk_jieba"。
bool IsKnownAnalyzer(const std::string& name);

// 按名称构造（schema 中的 analyzer 字符串）。未知名称返回 nullptr——
// schema 校验应先用 IsKnownAnalyzer 拦截，不允许静默回退。
std::unique_ptr<Analyzer> CreateAnalyzer(const std::string& name);

} // namespace pl::minisearch::analysis
