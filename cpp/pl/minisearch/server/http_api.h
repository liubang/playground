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

#include <brpc/controller.h>
#include <memory>
#include <optional>
#include <string>

#include "cpp/pl/minisearch/auth/console_auth.h"
#include "cpp/pl/minisearch/auth/keystore.h"
#include "cpp/pl/minisearch/embedding_client.h"
#include "cpp/pl/minisearch/proto/minisearch.pb.h"
#include "cpp/pl/minisearch/rerank_client.h"
#include "cpp/pl/minisearch/server/context.h"

namespace pl::minisearch::server {

// HTTP API (DESIGN.md §9/§10). Registered as "/api/v2/* => default_method"
// (plus /healthz). Requests authenticate via Bearer token when auth is on;
// /healthz is the only unauthenticated endpoint. Collections resolve within
// the principal's tenant. Embedding/rerank clients are optional; requests
// that need them degrade with a `degraded` marker instead of failing.
class HttpApiService : public proto::MiniSearchHttpService {
public:
    HttpApiService(TenantContext* context,
                   auth::KeyStore* keys,
                   bool auth_on,
                   std::shared_ptr<EmbeddingClient> embedding_client,
                   std::shared_ptr<RerankClient> rerank_client,
                   auth::ConsoleAuth* console_auth = nullptr,
                   std::string web_dir = "");

    void default_method(google::protobuf::RpcController* controller,
                        const proto::HttpRequest* /*request*/,
                        proto::HttpResponse* /*response*/,
                        google::protobuf::Closure* done) override;

private:
    // Returns the authenticated principal or nullopt (401 already sent).
    std::optional<auth::Principal> Authenticate(brpc::Controller* cntl);

    bool Allow(const auth::Principal& principal, const std::string& collection, bool write) const;

    void HandleCreateCollection(brpc::Controller* cntl, const auth::Principal&);
    void HandleListCollections(brpc::Controller* cntl, const auth::Principal&);
    void HandleDropCollection(brpc::Controller* cntl,
                              const auth::Principal&,
                              const std::string& name);
    void HandleUpsert(brpc::Controller* cntl,
                      const auth::Principal&,
                      const std::string& collection,
                      const std::string& id);
    void HandleGet(brpc::Controller* cntl,
                   const auth::Principal&,
                   const std::string& collection,
                   const std::string& id);
    void HandleListDocuments(brpc::Controller* cntl,
                             const auth::Principal&,
                             const std::string& collection);
    void HandleImport(brpc::Controller* cntl,
                      const auth::Principal&,
                      const std::string& collection);
    void HandleDelete(brpc::Controller* cntl,
                      const auth::Principal&,
                      const std::string& collection,
                      const std::string& id);
    void HandleSearch(brpc::Controller* cntl,
                      const auth::Principal&,
                      const std::string& collection);
    void HandleAnalyze(brpc::Controller* cntl,
                       const auth::Principal&,
                       const std::string& collection);
    void HandleHealthz(brpc::Controller* cntl);

    // Console session：账号密码登录（免 Bearer）与自服务（DESIGN.md §10 扩展）
    void HandleLogin(brpc::Controller* cntl);
    void HandleChangePassword(brpc::Controller* cntl);
    void HandleLogout(brpc::Controller* cntl);
    void HandleWhoAmI(brpc::Controller* cntl, const auth::Principal&);

    // Admin: /api/v2/admin/tenants... (DESIGN.md §10.5)
    void HandleCreateTenant(brpc::Controller* cntl, const auth::Principal&);
    void HandleListTenants(brpc::Controller* cntl, const auth::Principal&);
    void HandleDropTenant(brpc::Controller* cntl, const auth::Principal&, const std::string& name);
    void HandleIssueKey(brpc::Controller* cntl, const auth::Principal&, const std::string& tenant);
    void HandleListKeys(brpc::Controller* cntl, const auth::Principal&, const std::string& tenant);
    void HandleRevokeKey(brpc::Controller* cntl,
                         const auth::Principal&,
                         const std::string& tenant,
                         const std::string& key_id);
    void HandleStats(brpc::Controller* cntl, const auth::Principal&);

    // Server-side embedding（upsert / import 共用）：vec 字段 mode=server
    // 且文档未带向量时，对 source 字段文本（或 override 文本）做 embedding
    // 并写回 doc。失败时填充 status/error 并返回 false。
    bool ApplyServerEmbedding(const CollectionEntry& entry,
                              core::Document* doc,
                              const std::string* embed_text_override,
                              int* status,
                              std::string* error);

    template <typename T> bool ParseJsonBody(brpc::Controller* cntl, T* message);
    template <typename T>
    void SendJsonResponse(brpc::Controller* cntl, const T& message, int status_code = 200);
    void SendErrorResponse(brpc::Controller* cntl, int status_code, const std::string& error);

    TenantContext* context_;
    auth::KeyStore* keys_;
    bool auth_on_;
    std::shared_ptr<EmbeddingClient> embedding_client_;
    std::shared_ptr<RerankClient> rerank_client_;
    auth::ConsoleAuth* console_auth_; // nullable；session 登录链路
    std::string web_dir_;             // 空 = 不 serve console 静态文件
};

} // namespace pl::minisearch::server
