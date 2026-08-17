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

#include "cpp/pl/minisearch/rerank_client.h"

#include <brpc/channel.h>
#include <brpc/controller.h>
#include <butil/logging.h>
#include <butil/third_party/rapidjson/document.h>
#include <butil/third_party/rapidjson/stringbuffer.h>
#include <butil/third_party/rapidjson/writer.h>
#include <memory>

namespace rj = BUTIL_RAPIDJSON_NAMESPACE;

namespace pl::minisearch {

namespace {

class CohereRerankClient : public RerankClient {
public:
    explicit CohereRerankClient(const Options& options) : options_(options) {
        brpc::ChannelOptions ch_opts;
        ch_opts.protocol = brpc::PROTOCOL_HTTP;
        ch_opts.timeout_ms = options.timeout_ms;
        ch_opts.max_retry = options.max_retry;
        if (channel_.Init(options.endpoint.c_str(), &ch_opts) != 0) {
            LOG(ERROR) << "RerankClient: failed to init channel to " << options.endpoint;
        }
    }

    RerankResponse Rerank(const std::string& query,
                          const std::vector<std::string>& documents) override {
        RerankResponse resp;
        if (documents.empty()) {
            resp.ok = true;
            return resp;
        }

        // 请求体: {"model": "...", "query": "...", "documents": ["...", ...]}
        rj::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        doc.AddMember("model", rj::Value(options_.model.c_str(), alloc), alloc);
        doc.AddMember("query", rj::Value(query.c_str(), alloc), alloc);
        rj::Value arr(rj::kArrayType);
        for (const auto& d : documents) {
            arr.PushBack(rj::Value(d.c_str(), alloc), alloc);
        }
        doc.AddMember("documents", arr, alloc);
        if (options_.top_n > 0) {
            doc.AddMember("top_n", options_.top_n, alloc);
        }
        rj::StringBuffer buf;
        rj::Writer<rj::StringBuffer> writer(buf);
        doc.Accept(writer);

        // 发送 POST
        brpc::Controller cntl;
        cntl.http_request().uri() = options_.path;
        cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
        cntl.http_request().set_content_type("application/json");
        if (!options_.api_key.empty()) {
            cntl.http_request().SetHeader("Authorization", "Bearer " + options_.api_key);
        }
        cntl.request_attachment().append(buf.GetString());
        channel_.CallMethod(nullptr, &cntl, nullptr, nullptr, nullptr);

        if (cntl.Failed()) {
            resp.error = "HTTP request failed: " + cntl.ErrorText();
            return resp;
        }
        const int status = cntl.http_response().status_code();
        const std::string body = cntl.response_attachment().to_string();
        if (status != 200) {
            resp.error = "HTTP " + std::to_string(status) + ": " + body.substr(0, 200);
            return resp;
        }

        // 解析 Cohere 风格响应: {"results": [{"index": 0, "relevance_score": 0.9}, ...]}
        rj::Document json;
        if (json.Parse(body.c_str()).HasParseError()) {
            resp.error = "invalid JSON response";
            return resp;
        }
        if (!json.HasMember("results") || !json["results"].IsArray()) {
            resp.error = "response missing 'results' array";
            return resp;
        }
        const rj::Value& results = json["results"];
        for (rj::SizeType i = 0; i < results.Size(); ++i) {
            const rj::Value& item = results[i];
            if (item.HasMember("index") && item.HasMember("relevance_score")) {
                resp.results.push_back({static_cast<size_t>(item["index"].GetInt()),
                                        item["relevance_score"].GetDouble()});
            }
        }
        resp.ok = true;
        return resp;
    }

private:
    Options options_;
    brpc::Channel channel_;
};

} // namespace

std::unique_ptr<RerankClient> RerankClient::Create(const Options& options) {
    return std::make_unique<CohereRerankClient>(options);
}

} // namespace pl::minisearch
