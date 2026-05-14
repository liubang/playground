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
// Created: 2026/05/14 16:22

#pragma once

#include <memory>
#include <string>
#include <vector>

namespace pl::recall {

// Embedding 请求结果
struct EmbeddingResult {
    bool ok = false;
    std::string error;
    std::vector<float> embedding;
};

// 批量 Embedding 请求结果
struct BatchEmbeddingResult {
    bool ok = false;
    std::string error;
    // embeddings[i] 对应 texts[i]
    std::vector<std::vector<float>> embeddings;
};

// Embedding 客户端抽象接口
//
// 将文本编码为固定维度的 float 向量。具体实现可以对接 OpenAI API、
// 本地 sentence-transformers、vLLM、Ollama、HuggingFace TEI 等。
class EmbeddingClient {
public:
    virtual ~EmbeddingClient() = default;

    // 单条文本 → embedding
    virtual EmbeddingResult Embed(const std::string& text) = 0;

    // 批量文本 → embeddings（默认实现逐条调用，子类可覆盖为真正的 batch API）
    virtual BatchEmbeddingResult EmbedBatch(const std::vector<std::string>& texts);
};

// OpenAI 兼容的 Embedding 客户端
//
// 调用 POST /v1/embeddings 接口，兼容 OpenAI、Azure OpenAI、vLLM、
// Ollama (/v1/embeddings)、HuggingFace TEI 等所有实现了该接口的服务。
//
// 请求格式:
//   {"model": "<model>", "input": "<text>"}
//   {"model": "<model>", "input": ["<text1>", "<text2>", ...]}
//
// 响应格式:
//   {"data": [{"embedding": [0.1, 0.2, ...], "index": 0}, ...]}
class OpenAIEmbeddingClient : public EmbeddingClient {
public:
    struct Options {
        // Embedding 服务地址，如 "http://localhost:11434" 或 "https://api.openai.com"
        std::string endpoint;
        // API path，默认 "/v1/embeddings"
        std::string path = "/v1/embeddings";
        // 模型名称，如 "text-embedding-3-small"、"bge-m3" 等
        std::string model;
        // API key（可选，OpenAI 需要，本地服务通常不需要）
        std::string api_key;
        // 超时（毫秒）
        int timeout_ms = 30000;
        // 最大重试次数
        int max_retry = 2;
    };

    explicit OpenAIEmbeddingClient(Options options);
    ~OpenAIEmbeddingClient() override;

    EmbeddingResult Embed(const std::string& text) override;
    BatchEmbeddingResult EmbedBatch(const std::vector<std::string>& texts) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pl::recall
