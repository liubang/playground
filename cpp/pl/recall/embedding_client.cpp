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
// Created: 2026/05/14 16:21

#include "cpp/pl/recall/embedding_client.h"

#include <brpc/channel.h>
#include <brpc/controller.h>
#include <butil/logging.h>
#include <butil/third_party/rapidjson/document.h>
#include <butil/third_party/rapidjson/stringbuffer.h>
#include <butil/third_party/rapidjson/writer.h>

namespace rj = BUTIL_RAPIDJSON_NAMESPACE;

namespace pl::recall {

// =========================================================================
// EmbeddingClient 默认批量实现
// =========================================================================

BatchEmbeddingResult EmbeddingClient::EmbedBatch(const std::vector<std::string>& texts) {
    BatchEmbeddingResult result;
    result.embeddings.reserve(texts.size());
    for (const auto& text : texts) {
        auto single = Embed(text);
        if (!single.ok) {
            result.ok = false;
            result.error = "failed on text: " + single.error;
            return result;
        }
        result.embeddings.push_back(std::move(single.embedding));
    }
    result.ok = true;
    return result;
}

// =========================================================================
// OpenAIEmbeddingClient
// =========================================================================

struct OpenAIEmbeddingClient::Impl {
    Options options;
    brpc::Channel channel;

    bool Init() {
        brpc::ChannelOptions ch_opts;
        ch_opts.protocol = brpc::PROTOCOL_HTTP;
        ch_opts.timeout_ms = options.timeout_ms;
        ch_opts.max_retry = options.max_retry;
        if (channel.Init(options.endpoint.c_str(), &ch_opts) != 0) {
            LOG(ERROR) << "EmbeddingClient: failed to init channel to " << options.endpoint;
            return false;
        }
        return true;
    }

    // 构建单条请求 JSON: {"model": "...", "input": "..."}
    std::string BuildSingleRequestBody(const std::string& text) {
        rj::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        doc.AddMember("model", rj::Value(options.model.c_str(), alloc), alloc);
        doc.AddMember("input", rj::Value(text.c_str(), alloc), alloc);

        rj::StringBuffer buf;
        rj::Writer<rj::StringBuffer> writer(buf);
        doc.Accept(writer);
        return buf.GetString();
    }

    // 构建批量请求 JSON: {"model": "...", "input": ["...", "..."]}
    std::string BuildBatchRequestBody(const std::vector<std::string>& texts) {
        rj::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        doc.AddMember("model", rj::Value(options.model.c_str(), alloc), alloc);

        rj::Value arr(rj::kArrayType);
        for (const auto& t : texts) {
            arr.PushBack(rj::Value(t.c_str(), alloc), alloc);
        }
        doc.AddMember("input", arr, alloc);

        rj::StringBuffer buf;
        rj::Writer<rj::StringBuffer> writer(buf);
        doc.Accept(writer);
        return buf.GetString();
    }

    // 发送 HTTP POST 请求到 embedding endpoint
    std::string DoPost(const std::string& body, int& http_status) {
        brpc::Controller cntl;
        cntl.http_request().uri() = options.path;
        cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
        cntl.http_request().set_content_type("application/json");

        if (!options.api_key.empty()) {
            cntl.http_request().SetHeader("Authorization", "Bearer " + options.api_key);
        }

        cntl.request_attachment().append(body);
        channel.CallMethod(nullptr, &cntl, nullptr, nullptr, nullptr);

        if (cntl.Failed()) {
            http_status = -1;
            return "HTTP request failed: " + cntl.ErrorText();
        }

        http_status = cntl.http_response().status_code();
        return cntl.response_attachment().to_string();
    }

    // 从 OpenAI 格式的响应中解析 embedding 数组
    // 响应格式: {"data": [{"embedding": [...], "index": 0}, ...]}
    static bool ParseEmbeddings(const std::string& response_body,
                                std::vector<std::vector<float>>& out,
                                std::string& error) {
        rj::Document doc;
        if (doc.Parse(response_body.c_str()).HasParseError()) {
            error = "invalid JSON response";
            return false;
        }

        // 检查是否有 error 字段
        if (doc.HasMember("error")) {
            if (doc["error"].IsObject() && doc["error"].HasMember("message")) {
                error = doc["error"]["message"].GetString();
            } else if (doc["error"].IsString()) {
                error = doc["error"].GetString();
            } else {
                error = "unknown API error";
            }
            return false;
        }

        if (!doc.HasMember("data") || !doc["data"].IsArray()) {
            error = "response missing 'data' array";
            return false;
        }

        const auto& data = doc["data"];
        out.resize(data.Size());

        for (rj::SizeType i = 0; i < data.Size(); ++i) {
            const auto& item = data[i];
            if (!item.HasMember("embedding") || !item["embedding"].IsArray()) {
                error = "data[" + std::to_string(i) + "] missing 'embedding' array";
                return false;
            }

            // 确定输出位置：优先用 index 字段，否则按顺序
            int idx = static_cast<int>(i);
            if (item.HasMember("index") && item["index"].IsInt()) {
                idx = item["index"].GetInt();
            }
            if (idx < 0 || idx >= static_cast<int>(out.size())) {
                error = "invalid index " + std::to_string(idx);
                return false;
            }

            const auto& emb_arr = item["embedding"];
            auto& vec = out[static_cast<size_t>(idx)];
            vec.reserve(emb_arr.Size());
            for (rj::SizeType j = 0; j < emb_arr.Size(); ++j) {
                if (emb_arr[j].IsNumber()) {
                    vec.push_back(static_cast<float>(emb_arr[j].GetDouble()));
                }
            }
        }
        return true;
    }
};

OpenAIEmbeddingClient::OpenAIEmbeddingClient(Options options) : impl_(std::make_unique<Impl>()) {
    impl_->options = std::move(options);
    if (!impl_->Init()) {
        LOG(ERROR) << "EmbeddingClient: channel init failed";
    }
}

OpenAIEmbeddingClient::~OpenAIEmbeddingClient() = default;

EmbeddingResult OpenAIEmbeddingClient::Embed(const std::string& text) {
    EmbeddingResult result;

    auto body = impl_->BuildSingleRequestBody(text);
    int http_status = 0;
    auto response = impl_->DoPost(body, http_status);

    if (http_status < 0) {
        result.error = response;
        return result;
    }
    if (http_status != 200) {
        result.error = "HTTP " + std::to_string(http_status) + ": " + response;
        return result;
    }

    std::vector<std::vector<float>> embeddings;
    if (!Impl::ParseEmbeddings(response, embeddings, result.error)) {
        return result;
    }
    if (embeddings.empty()) {
        result.error = "empty embedding response";
        return result;
    }

    result.ok = true;
    result.embedding = std::move(embeddings[0]);
    return result;
}

BatchEmbeddingResult OpenAIEmbeddingClient::EmbedBatch(const std::vector<std::string>& texts) {
    BatchEmbeddingResult result;

    if (texts.empty()) {
        result.ok = true;
        return result;
    }

    auto body = impl_->BuildBatchRequestBody(texts);
    int http_status = 0;
    auto response = impl_->DoPost(body, http_status);

    if (http_status < 0) {
        result.error = response;
        return result;
    }
    if (http_status != 200) {
        result.error = "HTTP " + std::to_string(http_status) + ": " + response;
        return result;
    }

    if (!Impl::ParseEmbeddings(response, result.embeddings, result.error)) {
        return result;
    }
    if (result.embeddings.size() != texts.size()) {
        result.error = "embedding count mismatch: expected " + std::to_string(texts.size()) +
                       " got " + std::to_string(result.embeddings.size());
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace pl::recall
