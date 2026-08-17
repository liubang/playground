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
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpp/pl/minisearch/analysis/analyzer.h"

namespace pl::minisearch::index {

// M1 内存倒排（DESIGN.md §5.1）：term → posting list。
// 删除/替换不物理回收——查询侧按 tombstone 过滤，checkpoint 重建时压实。
class InvertedIndex {
public:
    struct Posting {
        int64_t docid;
        uint32_t tf;
    };

    // 写入一篇文档的分词结果（upsert 每次是新 docid，无需去重）。
    void Add(int64_t docid, const std::vector<analysis::Token>& tokens);

    std::vector<Posting> Find(const std::string& term) const;

    // BM25 统计（N 与 |d| 含未压实文档，M1 接受该近似）。
    int64_t DocCount() const;
    uint32_t DocLength(int64_t docid) const;
    double AvgDocLength() const;

    // 二进制持久化。M1 恢复路径不使用（重启时由 docstore 重分词重建，
    // 见 collection_registry.cpp LoadFromDisk）；为 M3 segment 落盘预留。
    bool Save(const std::string& path) const;
    bool Load(const std::string& path);

private:
    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, std::vector<Posting>> postings_;
    std::unordered_map<int64_t, uint32_t> doc_lengths_;
    int64_t total_length_ = 0;
};

} // namespace pl::minisearch::index
