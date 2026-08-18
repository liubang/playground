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

#include <atomic>
#include <brpc/server.h>
#include <chrono>
#include <csignal>
#include <gflags/gflags.h>
#include <memory>
#include <sys/stat.h>
#include <thread>

#include "cpp/pl/minisearch/auth/console_auth.h"
#include "cpp/pl/minisearch/auth/keystore.h"
#include "cpp/pl/minisearch/embedding_client.h"
#include "cpp/pl/minisearch/rerank_client.h"
#include "cpp/pl/minisearch/server/context.h"
#include "cpp/pl/minisearch/server/http_api.h"

DEFINE_int32(port, 8200, "TCP port of this server");
DEFINE_string(listen,
              "127.0.0.1",
              "Listen address; binding a non-loopback address requires --auth=true"
              " (fail-closed, DESIGN.md §10.4)");
DEFINE_int32(idle_timeout_s,
             -1,
             "Connection will be closed if there is no "
             "read/write operations during the last `idle_timeout_s`");
DEFINE_string(index_type, "Flat", "Faiss index type (Flat, IVF256_Flat, HNSW32, etc.)");
DEFINE_string(data_dir,
              "",
              "Persistence root; empty disables checkpoints and startup restore "
              "(collections live in-memory only). Required when --auth=true.");
DEFINE_bool(auth,
            false,
            "Require Bearer authentication on every endpoint except /healthz; "
            "off treats all requests as the default tenant's admin");

// Embedding 服务配置（可选；不配置时向量查询路降级并标记 degraded，
// server-embedded 向量字段写入返回 503）
DEFINE_string(embedding_endpoint,
              "",
              "Embedding service endpoint, e.g. http://localhost:8000 or https://api.openai.com");
DEFINE_string(embedding_model, "", "Embedding model name, e.g. bge-m3, text-embedding-3-small");
DEFINE_string(embedding_api_key, "", "API key for embedding service (optional for local services)");
DEFINE_string(embedding_path, "/v1/embeddings", "API path for embedding service");
DEFINE_int32(embedding_timeout_ms, 30000, "Embedding API timeout in milliseconds");

// Rerank 服务配置（可选；不配置时 rerank=true 的请求降级并标记 degraded）
DEFINE_string(rerank_endpoint, "", "Rerank service endpoint (Cohere-style /v1/rerank)");
DEFINE_string(rerank_model, "", "Rerank model name, e.g. bge-reranker-v2-m3");
DEFINE_string(rerank_api_key, "", "API key for rerank service (optional)");
DEFINE_string(rerank_path, "/v1/rerank", "API path for rerank service");
DEFINE_int32(rerank_timeout_ms, 30000, "Rerank API timeout in milliseconds");

// Console 控制台配置（可选）：账号密码登录 + 静态文件目录
DEFINE_string(web_dir,
              "",
              "Directory holding the console static files; empty disables /console/* serving");
DEFINE_string(console_admin_user,
              "",
              "Bootstrap console admin username; empty disables account/password login");
DEFINE_string(console_admin_password,
              "",
              "Bootstrap console admin password (only used to seed the store on first start)");

int main(int argc, char* argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    // 部署边界（DESIGN.md §10.4）：非回环监听必须开认证；认证需要 key 持久化目录。
    const bool loopback =
        FLAGS_listen == "127.0.0.1" || FLAGS_listen == "localhost" || FLAGS_listen == "::1";
    if (!loopback && !FLAGS_auth) {
        LOG(ERROR) << "refusing to start: non-loopback --listen requires --auth=true";
        return -1;
    }
    if (FLAGS_auth && FLAGS_data_dir.empty()) {
        LOG(ERROR) << "refusing to start: --auth=true requires --data_dir for key storage";
        return -1;
    }

    // 初始化 Embedding 客户端（可选）
    std::shared_ptr<pl::minisearch::EmbeddingClient> embedding_client;
    if (!FLAGS_embedding_endpoint.empty() && !FLAGS_embedding_model.empty()) {
        pl::minisearch::OpenAIEmbeddingClient::Options opts;
        opts.endpoint = FLAGS_embedding_endpoint;
        opts.model = FLAGS_embedding_model;
        opts.api_key = FLAGS_embedding_api_key;
        opts.path = FLAGS_embedding_path;
        opts.timeout_ms = FLAGS_embedding_timeout_ms;
        embedding_client = std::make_shared<pl::minisearch::OpenAIEmbeddingClient>(std::move(opts));
        LOG(INFO) << "Embedding client configured: endpoint=" << FLAGS_embedding_endpoint
                  << " model=" << FLAGS_embedding_model;
    } else {
        LOG(INFO) << "Embedding client not configured: vector query road will degrade "
                     "(BM25-only, marked degraded) and server-embedded upserts return 503";
    }

    // 初始化 Rerank 客户端（可选）
    std::shared_ptr<pl::minisearch::RerankClient> rerank_client;
    if (!FLAGS_rerank_endpoint.empty() && !FLAGS_rerank_model.empty()) {
        pl::minisearch::RerankClient::Options opts;
        opts.endpoint = FLAGS_rerank_endpoint;
        opts.model = FLAGS_rerank_model;
        opts.api_key = FLAGS_rerank_api_key;
        opts.path = FLAGS_rerank_path;
        opts.timeout_ms = FLAGS_rerank_timeout_ms;
        rerank_client = pl::minisearch::RerankClient::Create(opts);
        LOG(INFO) << "Rerank client configured: endpoint=" << FLAGS_rerank_endpoint
                  << " model=" << FLAGS_rerank_model;
    } else {
        LOG(INFO) << "Rerank client not configured: rerank=true requests will degrade "
                     "(RRF order kept, marked degraded)";
    }

    brpc::Server server;

    // key store（auth）：sha256 持久化 + 一次性 bootstrap admin key。
    // auth/ 目录在配置了 data_dir 时总是创建——console users 与 keys 共用该目录。
    if (!FLAGS_data_dir.empty()) {
        ::mkdir(FLAGS_data_dir.c_str(), 0755);
        ::mkdir((FLAGS_data_dir + "/auth").c_str(), 0755);
    }
    auto keys = std::make_unique<pl::minisearch::auth::KeyStore>(
        FLAGS_data_dir.empty() ? "" : FLAGS_data_dir + "/auth/keys.json");
    if (FLAGS_auth) {
        auto boot = keys->BootstrapIfNeeded(FLAGS_data_dir);
        if (boot.has_value()) {
            LOG(WARNING) << "bootstrapped admin key written to " << FLAGS_data_dir
                         << "/bootstrap.key (save it, then delete the file)";
        }
    }

    // console 账号密码登录（session）：仅配置了 bootstrap user 时启用；
    // users 持久化到 <data_dir>/auth/console_users.json（data_dir 为空时纯内存）
    pl::minisearch::auth::ConsoleAuth::Options console_opts;
    console_opts.path = FLAGS_data_dir.empty() ? "" : FLAGS_data_dir + "/auth/console_users.json";
    console_opts.bootstrap_user = FLAGS_console_admin_user;
    console_opts.bootstrap_password = FLAGS_console_admin_password;
    auto console_auth =
        std::make_unique<pl::minisearch::auth::ConsoleAuth>(std::move(console_opts));

    auto context =
        std::make_unique<pl::minisearch::server::TenantContext>(FLAGS_data_dir, FLAGS_index_type);
    const size_t loaded_tenants = context->LoadExistingTenants();
    if (loaded_tenants > 0) {
        LOG(INFO) << "Restored " << loaded_tenants << " tenant(s) from " << FLAGS_data_dir;
    }
    auto service = std::make_unique<pl::minisearch::server::HttpApiService>(context.get(),
                                                                            keys.get(),
                                                                            FLAGS_auth,
                                                                            embedding_client,
                                                                            rerank_client,
                                                                            console_auth.get(),
                                                                            FLAGS_web_dir);

    // /api/v2/* 通配路由 + /healthz（唯一免认证端点），路径段在
    // HttpApiService::default_method 内分发
    if (server.AddService(service.get(),
                          brpc::SERVER_DOESNT_OWN_SERVICE,
                          "/api/v2/* => default_method,"
                          "/healthz => default_method,"
                          "/console/* => default_method,"
                          "/ => default_method") != 0) {
        LOG(ERROR) << "Failed to add MiniSearchHttpService";
        return -1;
    }

    // brpc 不默认安装信号处理器：自行处理 SIGINT/SIGTERM，保证优雅退出时
    // context/registry 析构，checkpoint 线程完成 final flush。注意必须在
    // server.Start 之前安装——Start 内部可能有秒级阻塞（如主机名解析），
    // 期间收到的信号同样需要走优雅退出路径。
    static std::atomic<bool> quit{false};
    std::signal(SIGINT, [](int) { quit.store(true); });
    std::signal(SIGTERM, [](int) { quit.store(true); });

    brpc::ServerOptions options;
    options.idle_timeout_sec = FLAGS_idle_timeout_s;

    const std::string address = FLAGS_listen + ":" + std::to_string(FLAGS_port);
    if (server.Start(address.c_str(), &options) != 0) {
        LOG(ERROR) << "Failed to start server on " << address;
        return -1;
    }

    LOG(INFO) << "MiniSearch Server (HTTP/JSON) started on " << address
              << " auth=" << (FLAGS_auth ? "on" : "off") << " index_type=" << FLAGS_index_type
              << " console_login=" << (console_auth->enabled() ? "on" : "off")
              << " web_dir=" << (FLAGS_web_dir.empty() ? "(off)" : FLAGS_web_dir);

    while (!quit.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    LOG(INFO) << "shutting down (final checkpoint flush)...";
    server.Stop(0);
    server.Join();
    return 0;
}
