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
// Created: 2026/05/14 10:44

#include "cpp/pl/recall/embedding_client.h"
#include "cpp/pl/recall/recall_service.h"
#include <brpc/server.h>
#include <gflags/gflags.h>

DEFINE_int32(port, 8200, "TCP port of this server");
DEFINE_int32(idle_timeout_s,
             -1,
             "Connection will be closed if there is no "
             "read/write operations during the last `idle_timeout_s`");
DEFINE_int32(dimension, 768, "Embedding vector dimension");
DEFINE_string(index_type, "Flat", "Faiss index type (Flat, IVF256_Flat, HNSW32, etc.)");
DEFINE_string(snapshot_path, "", "Path to load snapshot from on startup");

// Embedding 服务配置（可选，不配置则 *_by_text 接口返回 503）
DEFINE_string(embedding_endpoint,
              "",
              "Embedding service endpoint, e.g. http://localhost:11434 or https://api.openai.com");
DEFINE_string(embedding_model, "", "Embedding model name, e.g. bge-m3, text-embedding-3-small");
DEFINE_string(embedding_api_key, "", "API key for embedding service (optional for local services)");
DEFINE_string(embedding_path, "/v1/embeddings", "API path for embedding service");
DEFINE_int32(embedding_timeout_ms, 30000, "Embedding API timeout in milliseconds");

int main(int argc, char* argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    // 初始化 Embedding 客户端（可选）
    std::shared_ptr<pl::recall::EmbeddingClient> embedding_client;
    if (!FLAGS_embedding_endpoint.empty() && !FLAGS_embedding_model.empty()) {
        pl::recall::OpenAIEmbeddingClient::Options opts;
        opts.endpoint = FLAGS_embedding_endpoint;
        opts.model = FLAGS_embedding_model;
        opts.api_key = FLAGS_embedding_api_key;
        opts.path = FLAGS_embedding_path;
        opts.timeout_ms = FLAGS_embedding_timeout_ms;
        embedding_client = std::make_shared<pl::recall::OpenAIEmbeddingClient>(std::move(opts));
        LOG(INFO) << "Embedding client configured: endpoint=" << FLAGS_embedding_endpoint
                  << " model=" << FLAGS_embedding_model;
    } else {
        LOG(INFO) << "Embedding client not configured, *_by_text APIs will return 503";
    }

    brpc::Server server;

    auto service = std::make_unique<pl::recall::RecallHttpServiceImpl>(
        FLAGS_dimension, FLAGS_index_type, embedding_client);

    // 启动时加载快照（如果指定了路径）
    if (!FLAGS_snapshot_path.empty()) {
        LOG(INFO) << "Loading snapshot from " << FLAGS_snapshot_path;
        if (service->Init(FLAGS_snapshot_path)) {
            LOG(INFO) << "Snapshot loaded successfully";
        } else {
            LOG(WARNING) << "Failed to load snapshot from " << FLAGS_snapshot_path;
        }
    }

    // 注册 HTTP 服务，通过 restful_mappings 将 /api/recall/* 路由到 default_method
    if (server.AddService(service.get(), brpc::SERVER_DOESNT_OWN_SERVICE,
                          "/api/recall/add              => default_method,"
                          "/api/recall/batch_add        => default_method,"
                          "/api/recall/search           => default_method,"
                          "/api/recall/add_by_text      => default_method,"
                          "/api/recall/batch_add_by_text => default_method,"
                          "/api/recall/search_by_text   => default_method,"
                          "/api/recall/snapshot/save    => default_method,"
                          "/api/recall/snapshot/load    => default_method,"
                          "/api/recall/stats            => default_method") != 0) {
        LOG(ERROR) << "Failed to add RecallHttpService";
        return -1;
    }

    brpc::ServerOptions options;
    options.idle_timeout_sec = FLAGS_idle_timeout_s;

    if (server.Start(FLAGS_port, &options) != 0) {
        LOG(ERROR) << "Failed to start server on port " << FLAGS_port;
        return -1;
    }

    LOG(INFO) << "VectorRecallService (HTTP/JSON) started on port " << FLAGS_port
              << " dimension=" << FLAGS_dimension << " index_type=" << FLAGS_index_type;

    server.RunUntilAskedToQuit();
    return 0;
}
