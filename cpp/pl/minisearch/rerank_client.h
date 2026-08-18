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

#include <memory>
#include <string>
#include <vector>

namespace pl::minisearch {

// Rerank 结果：文档在原始候选中的索引 + 相关性分数。
struct RerankResult {
    size_t index;
    double relevance_score;
};

struct RerankResponse {
    bool ok = false;
    std::string error;
    std::vector<RerankResult> results; // 按相关性降序
};

// Cohere 风格 rerank 客户端抽象
class RerankClient {
public:
    virtual ~RerankClient() = default;

    // query: 用户查询；documents: 候选文档文本列表。
    // 返回按相关性降序的 (index, score)，index 指向 documents 数组。
    virtual RerankResponse Rerank(const std::string& query,
                                  const std::vector<std::string>& documents) = 0;

    // Cohere 风格 HTTP 实现（OpenAI 兼容网关的 /v1/rerank 路径）。
    struct Options {
        std::string endpoint;
        std::string path = "/v1/rerank";
        std::string model;
        std::string api_key;
        int timeout_ms = 30000;
        int max_retry = 2;
        int top_n = 0; // 0 = 返回全部
    };

    static std::unique_ptr<RerankClient> Create(const Options& options);
};

} // namespace pl::minisearch
