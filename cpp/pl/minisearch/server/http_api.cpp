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

#include "cpp/pl/minisearch/server/http_api.h"

#include <algorithm>
#include <butil/logging.h>
#include <chrono>
#include <cstdlib>
#include <json2pb/json_to_pb.h>
#include <json2pb/pb_to_json.h>
#include <utility>
#include <vector>

#include "cpp/pl/minisearch/server/codec.h"
#include "cpp/pl/minisearch/server/filter.h"
#include "cpp/pl/minisearch/server/markdown.h"
#include "cpp/pl/minisearch/server/search_pipeline.h"
#include "cpp/pl/minisearch/server/static_files.h"

namespace pl::minisearch::server {

namespace {

constexpr size_t kRerankCandidates = 50;     // rerank 输入候选上限（DESIGN.md §7.1）
constexpr size_t kRerankTextMaxChars = 2000; // 单候选文本截断

std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start < path.size()) {
        const size_t slash = path.find('/', start);
        if (slash == std::string::npos) {
            parts.push_back(path.substr(start));
            break;
        }
        if (slash > start) {
            parts.push_back(path.substr(start, slash - start));
        }
        start = slash + 1;
    }
    return parts;
}

// parts[from..] 用 '/' 重新拼接（文档 id 允许包含 '/'，DESIGN.md §4.2）。
std::string join_path(const std::vector<std::string>& parts, size_t from) {
    std::string out;
    for (size_t i = from; i < parts.size(); ++i) {
        if (i > from) {
            out.push_back('/');
        }
        out += parts[i];
    }
    return out;
}

// URL 百分号解码（%XX -> 字节）。brpc 的 uri().path() 返回原始未解码路径，
// 而文档 id 可能含 '#'（markdown 导入的 chunk id 形如 "<name>#chunk_<i>"），
// 前端必须编码后传输，这里解码还原。非法编码原样保留。
std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                    return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(s[i + 1]);
            const int lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

// Copies a repeated float field into std::vector<float> (see codec.cpp for the
// ASan note on element-wise access).
std::vector<float> copy_floats(const google::protobuf::RepeatedField<float>& field) {
    std::vector<float> out;
    out.reserve(field.size());
    for (int i = 0; i < field.size(); ++i) {
        out.push_back(field.Get(i));
    }
    return out;
}

bool is_admin(const auth::Principal& p) {
    return p.role == auth::Role::kAdmin;
}

bool is_tenant_admin_or_above(const auth::Principal& p) {
    return p.role == auth::Role::kAdmin || p.role == auth::Role::kTenantAdmin;
}

// rerank 输入文本：拼接文档的全部 stored text 字段并截断。
std::string rerank_text(const core::Document& doc, const core::Schema& schema) {
    std::string text;
    for (const auto& [name, def] : schema.fields) {
        if (def.type != core::FieldType::kText) {
            continue;
        }
        const auto it = doc.fields.find(name);
        if (it == doc.fields.end() || !std::holds_alternative<std::string>(it->second)) {
            continue;
        }
        if (!text.empty()) {
            text.push_back(' ');
        }
        text += std::get<std::string>(it->second);
        if (text.size() >= kRerankTextMaxChars) {
            text.resize(kRerankTextMaxChars);
            break;
        }
    }
    return text;
}

} // namespace

HttpApiService::HttpApiService(TenantContext* context,
                               auth::KeyStore* keys,
                               bool auth_on,
                               std::shared_ptr<EmbeddingClient> embedding_client,
                               std::shared_ptr<RerankClient> rerank_client,
                               auth::ConsoleAuth* console_auth,
                               std::string web_dir)
    : context_(context),
      keys_(keys),
      auth_on_(auth_on),
      embedding_client_(std::move(embedding_client)),
      rerank_client_(std::move(rerank_client)),
      console_auth_(console_auth),
      web_dir_(std::move(web_dir)) {}

std::optional<auth::Principal> HttpApiService::Authenticate(brpc::Controller* cntl) {
    if (!auth_on_) {
        auth::Principal principal;
        principal.tenant = "default";
        principal.role = auth::Role::kAdmin;
        principal.key_id = "anonymous";
        return principal;
    }
    const std::string* header = cntl->http_request().GetHeader("Authorization");
    const std::string prefix = "Bearer ";
    if (header == nullptr || header->rfind(prefix, 0) != 0) {
        SendErrorResponse(cntl, 401, "missing bearer token");
        return std::nullopt;
    }
    const std::string token = header->substr(prefix.size());
    auto principal = keys_->Authenticate(token);
    if (!principal.has_value() && console_auth_ != nullptr) {
        // session token（console 账号密码登录）与 API key 共用 Bearer 头
        principal = console_auth_->Authenticate(token);
    }
    if (!principal.has_value()) {
        SendErrorResponse(cntl, 401, "invalid or revoked token");
        return std::nullopt;
    }
    return principal;
}

bool HttpApiService::Allow(const auth::Principal& principal,
                           const std::string& collection,
                           bool write) const {
    // 角色判定：reader 不能写；其余角色（含 writer）读写在角色层放行，
    // 然后统一应用 collection 白名单（DESIGN.md §10.2：writer/reader
    // 均可被白名单收窄）。
    switch (principal.role) {
        case auth::Role::kAdmin:
        case auth::Role::kTenantAdmin:
        case auth::Role::kWriter:
            break;
        case auth::Role::kReader:
            if (write) {
                return false;
            }
            break;
    }
    if (principal.collections.empty()) {
        return true;
    }
    return std::find(principal.collections.begin(), principal.collections.end(), collection) !=
           principal.collections.end();
}

void HttpApiService::default_method(google::protobuf::RpcController* controller,
                                    const proto::HttpRequest* /*request*/,
                                    proto::HttpResponse* /*response*/,
                                    google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    auto* cntl = static_cast<brpc::Controller*>(controller);
    const brpc::HttpMethod method = cntl->http_request().method();
    const std::string path = cntl->http_request().uri().path();

    if (path == "/healthz" && method == brpc::HTTP_METHOD_GET) {
        return HandleHealthz(cntl);
    }

    // console 静态文件：免认证（登录页本身需要加载）
    if (!web_dir_.empty() && method == brpc::HTTP_METHOD_GET) {
        // "/" 与 "/console"（无尾斜杠）统一 302 到 "/console/"，
        // 由 ServeStaticFile 补 index.html；否则 /console 会落入认证分支返回 401。
        if (path == "/" || path == "/console") {
            cntl->http_response().set_status_code(302);
            cntl->http_response().SetHeader("Location", "/console/");
            return;
        }
        if (path.rfind("/console/", 0) == 0) {
            if (!ServeStaticFile(web_dir_, path.substr(9), cntl)) {
                SendErrorResponse(cntl, 404, "not found: " + path);
            }
            return;
        }
    }
    // 账号密码登录与修改密码：免 Bearer（凭据本身即认证）
    if (path == "/api/v2/auth/login" && method == brpc::HTTP_METHOD_POST) {
        return HandleLogin(cntl);
    }
    if (path == "/api/v2/auth/password" && method == brpc::HTTP_METHOD_POST) {
        return HandleChangePassword(cntl);
    }

    auto principal = Authenticate(cntl);
    if (!principal.has_value()) {
        return; // 401 已发送
    }
    // admin 可用 ?tenant= 覆盖目标租户（跨租户访问，DESIGN.md §10.2）。
    if (is_admin(*principal)) {
        if (const std::string* override_tenant = cntl->http_request().uri().GetQuery("tenant")) {
            if (!core::IsValidResourceName(*override_tenant)) {
                return SendErrorResponse(cntl, 400, "invalid tenant name");
            }
            principal->tenant = *override_tenant;
        }
    }

    const std::vector<std::string> parts = split_path(path);
    // parts[0]="api", parts[1]="v2" (restful prefix "/api/v2/*").
    if (parts.size() < 2 || parts[1] != "v2") {
        SendErrorResponse(cntl, 404, "not found: " + path);
        return;
    }

    if (parts.size() == 4 && parts[2] == "auth") {
        if (parts[3] == "logout" && method == brpc::HTTP_METHOD_POST) {
            return HandleLogout(cntl);
        }
        if (parts[3] == "whoami" && method == brpc::HTTP_METHOD_GET) {
            return HandleWhoAmI(cntl, *principal);
        }
        SendErrorResponse(cntl, 404, "not found: " + path);
        return;
    }

    if (parts.size() >= 3 && parts[2] == "admin") {
        if (parts.size() == 4 && parts[3] == "tenants" && method == brpc::HTTP_METHOD_GET) {
            return HandleListTenants(cntl, *principal);
        }
        if (parts.size() == 4 && parts[3] == "tenants" && method == brpc::HTTP_METHOD_POST) {
            return HandleCreateTenant(cntl, *principal);
        }
        if (parts.size() == 5 && parts[3] == "tenants" && method == brpc::HTTP_METHOD_DELETE) {
            return HandleDropTenant(cntl, *principal, parts[4]);
        }
        if (parts.size() == 6 && parts[3] == "tenants" && parts[5] == "keys") {
            if (method == brpc::HTTP_METHOD_POST) {
                return HandleIssueKey(cntl, *principal, parts[4]);
            }
            if (method == brpc::HTTP_METHOD_GET) {
                return HandleListKeys(cntl, *principal, parts[4]);
            }
        }
        // /admin/tenants/{src}/keys/{key_id}:move —— API key 跨租户迁移。
        // key_id 形如 "k_<n>"，不含 ':'，后缀匹配无歧义。
        if (parts.size() == 7 && parts[3] == "tenants" && parts[5] == "keys" &&
            method == brpc::HTTP_METHOD_POST && parts[6].ends_with(":move")) {
            return HandleMoveKey(
                cntl, *principal, parts[4], parts[6].substr(0, parts[6].size() - 5));
        }
        if (parts.size() == 7 && parts[3] == "tenants" && parts[5] == "keys" &&
            method == brpc::HTTP_METHOD_DELETE) {
            return HandleRevokeKey(cntl, *principal, parts[4], parts[6]);
        }
        // /admin/tenants/{src}/collections/{name}:move —— collection 跨租户迁移。
        // collection 名字符集为 [A-Za-z0-9_-]，不含 ':'，后缀匹配无歧义。
        if (parts.size() == 7 && parts[3] == "tenants" && parts[5] == "collections" &&
            method == brpc::HTTP_METHOD_POST && parts[6].ends_with(":move")) {
            return HandleMoveCollection(
                cntl, *principal, parts[4], parts[6].substr(0, parts[6].size() - 5));
        }
        if (parts.size() == 4 && parts[3] == "stats" && method == brpc::HTTP_METHOD_GET) {
            return HandleStats(cntl, *principal);
        }
        SendErrorResponse(cntl, 404, "not found: " + path);
        return;
    }

    if (parts.size() == 3 && parts[2] == "collections") {
        if (method == brpc::HTTP_METHOD_POST) {
            if (!is_tenant_admin_or_above(*principal)) {
                return SendErrorResponse(cntl, 403, "tenant_admin role required");
            }
            return HandleCreateCollection(cntl, *principal);
        }
        if (method == brpc::HTTP_METHOD_GET) {
            return HandleListCollections(cntl, *principal);
        }
    }
    if (parts.size() == 4 && parts[2] == "collections" && method == brpc::HTTP_METHOD_DELETE) {
        if (!is_tenant_admin_or_above(*principal)) {
            return SendErrorResponse(cntl, 403, "tenant_admin role required");
        }
        return HandleDropCollection(cntl, *principal, parts[3]);
    }
    // 文档路由：id 允许包含 '/'，路径剩余部分整体作为 id。
    if (parts.size() >= 5 && parts[3] == "documents") {
        // path 未解码（brpc uri().path() 返回原始字节），%23 等需还原，
        // 否则含 '#' 的 chunk id（markdown 导入）无法定位。
        const std::string id = url_decode(join_path(parts, 4));
        if (!Allow(*principal, parts[2], /*write=*/method != brpc::HTTP_METHOD_GET)) {
            return SendErrorResponse(cntl, 403, "forbidden");
        }
        if (method == brpc::HTTP_METHOD_PUT) {
            return HandleUpsert(cntl, *principal, parts[2], id);
        }
        if (method == brpc::HTTP_METHOD_GET) {
            return HandleGet(cntl, *principal, parts[2], id);
        }
        if (method == brpc::HTTP_METHOD_DELETE) {
            return HandleDelete(cntl, *principal, parts[2], id);
        }
    }
    if (parts.size() == 4 && parts[3] == "documents" && method == brpc::HTTP_METHOD_GET) {
        if (!Allow(*principal, parts[2], /*write=*/false)) {
            return SendErrorResponse(cntl, 403, "forbidden");
        }
        return HandleListDocuments(cntl, *principal, parts[2]);
    }
    if (parts.size() == 4 && parts[3] == "documents:import" && method == brpc::HTTP_METHOD_POST) {
        if (!Allow(*principal, parts[2], /*write=*/true)) {
            return SendErrorResponse(cntl, 403, "forbidden");
        }
        return HandleImport(cntl, *principal, parts[2]);
    }
    if (parts.size() == 4 && parts[3] == "search" && method == brpc::HTTP_METHOD_POST) {
        if (!Allow(*principal, parts[2], /*write=*/false)) {
            return SendErrorResponse(cntl, 403, "forbidden");
        }
        return HandleSearch(cntl, *principal, parts[2]);
    }
    if (parts.size() == 4 && parts[3] == "queries:analyze" && method == brpc::HTTP_METHOD_POST) {
        if (!Allow(*principal, parts[2], /*write=*/false)) {
            return SendErrorResponse(cntl, 403, "forbidden");
        }
        return HandleAnalyze(cntl, *principal, parts[2]);
    }
    SendErrorResponse(cntl, 404, "not found: " + path);
}

void HttpApiService::HandleHealthz(brpc::Controller* cntl) {
    cntl->http_response().set_status_code(200);
    cntl->http_response().set_content_type("application/json");
    cntl->response_attachment().append(R"({"status":"ok"})");
}

// ---------------------------------------------------------------------------
// collections / documents / search（租户内路由）
// ---------------------------------------------------------------------------

void HttpApiService::HandleCreateCollection(brpc::Controller* cntl,
                                            const auth::Principal& principal) {
    proto::CollectionSpec spec;
    if (!ParseJsonBody(cntl, &spec)) {
        return;
    }
    // 请求本身非法（名称/schema）-> 400；与现有 collection 冲突 -> 409。
    if (!core::IsValidResourceName(spec.name())) {
        SendErrorResponse(cntl, 400, "collection name must match [A-Za-z0-9_-]{1,64}");
        return;
    }
    {
        core::Schema schema;
        std::string error;
        if (!ToCoreSchema(spec, &schema, &error)) {
            SendErrorResponse(cntl, 400, error);
            return;
        }
    }
    auto registry = context_->Registry(principal.tenant);
    if (registry == nullptr) {
        SendErrorResponse(cntl, 400, "invalid tenant: " + principal.tenant);
        return;
    }
    auto result = registry->Create(spec);
    proto::GenericResponse resp;
    resp.set_ok(result.ok);
    resp.set_error(result.error);
    SendJsonResponse(cntl, resp, result.ok ? 200 : 409);
}

void HttpApiService::HandleListCollections(brpc::Controller* cntl,
                                           const auth::Principal& principal) {
    auto registry = context_->Registry(principal.tenant);
    if (registry == nullptr) {
        SendErrorResponse(cntl, 400, "invalid tenant: " + principal.tenant);
        return;
    }
    proto::ListCollectionsResponse resp;
    for (const auto& [name, count] : registry->ListWithCounts()) {
        auto* entry = resp.add_collections();
        entry->set_name(name);
        entry->set_active_documents(static_cast<int64_t>(count));
    }
    SendJsonResponse(cntl, resp);
}

void HttpApiService::HandleDropCollection(brpc::Controller* cntl,
                                          const auth::Principal& principal,
                                          const std::string& name) {
    const std::string* confirm = cntl->http_request().uri().GetQuery("confirm");
    if (confirm == nullptr || *confirm != name) {
        SendErrorResponse(cntl, 400, "confirm query parameter must equal the collection name");
        return;
    }
    auto registry = context_->Registry(principal.tenant);
    if (registry == nullptr) {
        SendErrorResponse(cntl, 400, "invalid tenant: " + principal.tenant);
        return;
    }
    proto::GenericResponse resp;
    resp.set_ok(registry->Drop(name));
    SendJsonResponse(cntl, resp, resp.ok() ? 200 : 404);
}

void HttpApiService::HandleUpsert(brpc::Controller* cntl,
                                  const auth::Principal& principal,
                                  const std::string& collection,
                                  const std::string& id) {
    auto registry = context_->Registry(principal.tenant);
    if (registry == nullptr) {
        SendErrorResponse(cntl, 400, "invalid tenant: " + principal.tenant);
        return;
    }
    auto entry = registry->Find(collection);
    if (entry == nullptr) {
        SendErrorResponse(cntl, 404, "unknown collection: " + collection);
        return;
    }
    proto::Document body;
    if (!ParseJsonBody(cntl, &body)) {
        return;
    }
    body.set_id(id); // path wins

    core::Document doc;
    std::string error;
    if (!ToCoreDocument(body, &doc, &error)) {
        SendErrorResponse(cntl, 400, error);
        return;
    }

    // Server-side embedding for vector fields with mode="server".
    {
        int status = 0;
        std::string embed_error;
        if (!ApplyServerEmbedding(*entry, &doc, nullptr, &status, &embed_error)) {
            SendErrorResponse(cntl, status, embed_error);
            return;
        }
    }

    auto upserted = entry->docs.Upsert(std::move(doc));
    proto::UpsertResponse resp;
    resp.set_ok(upserted.ok);
    resp.set_error(upserted.error);
    resp.set_internal_docid(upserted.internal_docid);
    resp.set_superseded_docid(upserted.superseded_docid);
    if (!upserted.ok) {
        SendJsonResponse(
            cntl, resp, upserted.error.find("stale version") == std::string::npos ? 400 : 409);
        return;
    }
    // 索引写入跟随文档写入；失败不阻断（文档仍可按 id 检索，崩溃一致性
    // 窗口见 DESIGN.md §5.1，M0 无 WAL）。失败细节见 IndexVector 的日志。
    core::Document stored;
    if (entry->docs.GetByInternal(upserted.internal_docid, &stored)) {
        entry->IndexVector(upserted.internal_docid, stored);
        entry->IndexText(upserted.internal_docid, stored);
    }
    SendJsonResponse(cntl, resp);
}

bool HttpApiService::ApplyServerEmbedding(const CollectionEntry& entry,
                                          core::Document* doc,
                                          const std::string* embed_text_override,
                                          int* status,
                                          std::string* error) {
    const core::FieldDef* vec = CollectionRegistry::VectorField(entry.docs.schema());
    if (vec == nullptr || !vec->server_embedded || doc->fields.count(vec->name) != 0) {
        return true;
    }
    std::string text;
    if (embed_text_override != nullptr) {
        text = *embed_text_override;
    } else {
        const auto it = doc->fields.find(vec->source_field);
        if (it == doc->fields.end()) {
            *status = 400;
            *error = "source field absent: " + vec->source_field;
            return false;
        }
        text = std::get<std::string>(it->second);
    }
    if (embedding_client_ == nullptr) {
        *status = 503;
        *error = "embedding service not configured";
        return false;
    }
    auto result = embedding_client_->Embed(text);
    if (!result.ok) {
        *status = 502;
        *error = "embedding failed: " + result.error;
        return false;
    }
    doc->fields[vec->name] = std::move(result.embedding);
    return true;
}

void HttpApiService::HandleGet(brpc::Controller* cntl,
                               const auth::Principal& principal,
                               const std::string& collection,
                               const std::string& id) {
    auto registry = context_->Registry(principal.tenant);
    if (registry == nullptr) {
        SendErrorResponse(cntl, 400, "invalid tenant: " + principal.tenant);
        return;
    }
    auto entry = registry->Find(collection);
    if (entry == nullptr) {
        SendErrorResponse(cntl, 404, "unknown collection: " + collection);
        return;
    }
    proto::GetDocumentResponse resp;
    core::Document doc;
    if (entry->docs.Get(id, &doc)) {
        resp.set_found(true);
        ToProtoDocument(doc, resp.mutable_document(), /*include_internal=*/false);
    }
    SendJsonResponse(cntl, resp, resp.found() ? 200 : 404);
}

void HttpApiService::HandleDelete(brpc::Controller* cntl,
                                  const auth::Principal& principal,
                                  const std::string& collection,
                                  const std::string& id) {
    auto registry = context_->Registry(principal.tenant);
    if (registry == nullptr) {
        SendErrorResponse(cntl, 400, "invalid tenant: " + principal.tenant);
        return;
    }
    auto entry = registry->Find(collection);
    if (entry == nullptr) {
        SendErrorResponse(cntl, 404, "unknown collection: " + collection);
        return;
    }
    proto::GenericResponse resp;
    resp.set_ok(entry->docs.Delete(id));
    SendJsonResponse(cntl, resp, resp.ok() ? 200 : 404);
}

void HttpApiService::HandleListDocuments(brpc::Controller* cntl,
                                         const auth::Principal& principal,
                                         const std::string& collection) {
    auto registry = context_->Registry(principal.tenant);
    if (registry == nullptr) {
        SendErrorResponse(cntl, 400, "invalid tenant: " + principal.tenant);
        return;
    }
    auto entry = registry->Find(collection);
    if (entry == nullptr) {
        SendErrorResponse(cntl, 404, "unknown collection: " + collection);
        return;
    }
    size_t offset = 0;
    size_t limit = 50;
    if (const std::string* q = cntl->http_request().uri().GetQuery("offset"); q != nullptr) {
        long v = std::atol(q->c_str());
        offset = v > 0 ? static_cast<size_t>(v) : 0;
    }
    if (const std::string* q = cntl->http_request().uri().GetQuery("limit"); q != nullptr) {
        long v = std::atol(q->c_str());
        if (v > 0)
            limit = static_cast<size_t>(v);
    }
    limit = std::min(limit, static_cast<size_t>(200));
    size_t total = 0;
    auto docs = entry->docs.ListDocuments(offset, limit, &total);
    proto::ListDocumentsResponse resp;
    resp.set_total(static_cast<int64_t>(total));
    const core::FieldDef* vec = CollectionRegistry::VectorField(entry->docs.schema());
    for (const auto& doc : docs) {
        auto* out = resp.add_documents();
        ToProtoDocument(doc, out, /*include_internal=*/false);
        if (vec != nullptr) {
            out->mutable_fields()->erase(vec->name); // 向量不出现在列表响应
        }
    }
    SendJsonResponse(cntl, resp);
}

void HttpApiService::HandleImport(brpc::Controller* cntl,
                                  const auth::Principal& principal,
                                  const std::string& collection) {
    auto registry = context_->Registry(principal.tenant);
    if (registry == nullptr) {
        SendErrorResponse(cntl, 400, "invalid tenant: " + principal.tenant);
        return;
    }
    auto entry = registry->Find(collection);
    if (entry == nullptr) {
        SendErrorResponse(cntl, 404, "unknown collection: " + collection);
        return;
    }
    proto::ImportDocumentsRequest req;
    if (!ParseJsonBody(cntl, &req)) {
        return;
    }
    if (req.name().empty() || req.name().find('#') != std::string::npos) {
        SendErrorResponse(cntl, 400, "name must be non-empty and must not contain '#'");
        return;
    }
    if (req.content().empty()) {
        SendErrorResponse(cntl, 400, "content must not be empty");
        return;
    }
    const core::Schema& schema = entry->docs.schema();

    // 正文字段：显式指定 > body > content > 首个 text 字段
    std::string text_field = req.text_field();
    if (text_field.empty()) {
        if (schema.Find("body") != nullptr && schema.Find("body")->type == core::FieldType::kText) {
            text_field = "body";
        } else if (schema.Find("content") != nullptr &&
                   schema.Find("content")->type == core::FieldType::kText) {
            text_field = "content";
        } else {
            for (const auto& [name, def] : schema.fields) {
                if (def.type == core::FieldType::kText) {
                    text_field = name;
                    break;
                }
            }
        }
    }
    const core::FieldDef* text_def = schema.Find(text_field);
    if (text_def == nullptr || text_def->type != core::FieldType::kText) {
        SendErrorResponse(cntl, 400, "no usable text field: " + text_field);
        return;
    }
    // 标题路径字段：显式指定 > 存在 "title" 字段
    std::string title_field = req.title_field();
    if (title_field.empty() && schema.Find("title") != nullptr) {
        title_field = "title";
    }
    if (!title_field.empty()) {
        const core::FieldDef* def = schema.Find(title_field);
        if (def == nullptr ||
            (def->type != core::FieldType::kText && def->type != core::FieldType::kKeyword)) {
            SendErrorResponse(cntl, 400, "invalid title field: " + title_field);
            return;
        }
    }

    ChunkOptions opts;
    if (req.chunk_size() > 0)
        opts.max_chars = static_cast<size_t>(req.chunk_size());
    if (req.chunk_overlap() >= 0)
        opts.overlap_chars = static_cast<size_t>(req.chunk_overlap());
    const std::string strategy = req.strategy().empty() ? "markdown" : req.strategy();
    std::vector<MarkdownChunk> chunks;
    if (strategy == "markdown") {
        chunks = ChunkMarkdown(req.content(), opts);
    } else if (strategy == "fixed") {
        chunks = ChunkFixed(req.content(), opts);
    } else {
        SendErrorResponse(cntl, 400, "unknown strategy: " + strategy);
        return;
    }
    if (chunks.empty()) {
        SendErrorResponse(cntl, 400, "content produced no chunks");
        return;
    }

    // 幂等重导：按 id 前缀删掉同名文档的旧 chunk
    const std::string prefix = req.name() + "#chunk_";
    std::vector<std::string> stale;
    entry->docs.ForEachActive([&](const core::Document& doc) {
        if (doc.id.rfind(prefix, 0) == 0)
            stale.push_back(doc.id);
    });
    for (const auto& id : stale) {
        entry->docs.Delete(id);
    }

    const core::FieldDef* vec = CollectionRegistry::VectorField(schema);
    proto::ImportDocumentsResponse resp;
    resp.set_ok(true);
    for (size_t i = 0; i < chunks.size(); ++i) {
        proto::Document body;
        body.set_id(prefix + std::to_string(i));
        (*body.mutable_fields())[text_field].set_s(chunks[i].text);
        if (!title_field.empty() && !chunks[i].title_path.empty()) {
            (*body.mutable_fields())[title_field].set_s(chunks[i].title_path);
        }
        for (const auto& [k, v] : req.fields()) {
            if (!body.mutable_fields()->count(k)) {
                (*body.mutable_fields())[k] = v;
            }
        }
        core::Document doc;
        std::string error;
        if (!ToCoreDocument(body, &doc, &error)) {
            resp.set_ok(false);
            resp.set_error(error);
            SendJsonResponse(cntl, resp, 400);
            return;
        }
        // 向量 embedding 输入带标题前缀（轻量 contextual），存储正文不含前缀。
        std::string embed_text;
        const std::string* override_ptr = nullptr;
        if (vec != nullptr && vec->server_embedded && vec->source_field == text_field) {
            embed_text = chunks[i].title_path.empty()
                             ? chunks[i].text
                             : chunks[i].title_path + "\n" + chunks[i].text;
            override_ptr = &embed_text;
        }
        int status = 0;
        if (!ApplyServerEmbedding(*entry, &doc, override_ptr, &status, &error)) {
            resp.set_ok(false);
            resp.set_error(error);
            SendJsonResponse(cntl, resp, status);
            return;
        }
        auto upserted = entry->docs.Upsert(std::move(doc));
        if (!upserted.ok) {
            resp.set_ok(false);
            resp.set_error(upserted.error);
            SendJsonResponse(cntl, resp, 400);
            return;
        }
        core::Document stored;
        if (entry->docs.GetByInternal(upserted.internal_docid, &stored)) {
            entry->IndexVector(upserted.internal_docid, stored);
            entry->IndexText(upserted.internal_docid, stored);
        }
        resp.add_document_ids(prefix + std::to_string(i));
    }
    resp.set_chunks(static_cast<int64_t>(chunks.size()));
    SendJsonResponse(cntl, resp);
}

void HttpApiService::HandleSearch(brpc::Controller* cntl,
                                  const auth::Principal& principal,
                                  const std::string& collection) {
    const auto start = std::chrono::steady_clock::now();
    auto registry = context_->Registry(principal.tenant);
    if (registry == nullptr) {
        SendErrorResponse(cntl, 400, "invalid tenant: " + principal.tenant);
        return;
    }
    auto entry = registry->Find(collection);
    if (entry == nullptr) {
        SendErrorResponse(cntl, 404, "unknown collection: " + collection);
        return;
    }
    proto::SearchRequest req;
    if (!ParseJsonBody(cntl, &req)) {
        return;
    }

    // 请求级 weights 覆盖默认（DESIGN.md §9.2）：缺省 1.0/1.0，显式 0 关闭该路。
    HybridOptions options;
    if (req.has_weights()) {
        options.bm25_weight = req.weights().bm25();
        options.vector_weight = req.weights().vector();
    }
    if (options.bm25_weight < 0.0 || options.vector_weight < 0.0) {
        SendErrorResponse(cntl, 400, "weights must be non-negative");
        return;
    }

    // 后置 filter（DESIGN.md §7.3 M1）
    std::function<bool(const core::Document&)> filter;
    if (req.has_filter()) {
        if (auto err = ValidateFilter(req.filter(), entry->docs.schema()); err.has_value()) {
            SendErrorResponse(cntl, 400, *err);
            return;
        }
        filter = BuildFilterPredicate(req.filter());
    }

    // 向量路 query：embedding 直传或 text 服务端 embedding。
    std::vector<std::string> degraded;
    std::vector<float> query_embedding = copy_floats(req.embedding());
    const bool want_vector = entry->index != nullptr && options.vector_weight > 0.0 &&
                             (!req.text().empty() || !query_embedding.empty());
    if (query_embedding.empty() && !req.text().empty() && entry->index != nullptr &&
        options.vector_weight > 0.0) {
        if (embedding_client_ != nullptr) {
            auto result = embedding_client_->Embed(req.text());
            if (result.ok) {
                query_embedding = std::move(result.embedding);
            } else {
                LOG(WARNING) << "query embedding failed, degrading to BM25-only: " << result.error;
            }
        }
        // embedding 不可用不阻断：BM25 路仍可用（降级语义，DESIGN.md §7.5）
    }
    if (!query_embedding.empty() && entry->index != nullptr &&
        static_cast<int>(query_embedding.size()) != entry->index->dimension()) {
        SendErrorResponse(cntl, 400, "query dims mismatch");
        return;
    }

    const int top_k = req.top_k() > 0 ? req.top_k() : 10;
    auto result = HybridSearch(entry->QueryAnalyzer(),
                               *entry->inverted,
                               entry->index.get(),
                               entry->docs,
                               req.text(),
                               query_embedding,
                               top_k,
                               options,
                               filter);
    if (want_vector && !result.vector_active) {
        degraded.push_back("vector");
    }

    // 可选 rerank（DESIGN.md §7.1 第 4 步）：融合后 top-N 交 cross-encoder
    // 重打分；未配置或调用失败时保持 RRF 序并标记 degraded。
    if (req.rerank()) {
        if (rerank_client_ == nullptr) {
            degraded.push_back("rerank");
        } else if (result.hits.size() > 1 && !req.text().empty()) {
            const size_t n = std::min(result.hits.size(), kRerankCandidates);
            std::vector<std::string> texts;
            texts.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                core::Document doc;
                if (entry->docs.GetByInternal(result.hits[i].docid, &doc)) {
                    texts.push_back(rerank_text(doc, entry->docs.schema()));
                } else {
                    texts.emplace_back();
                }
            }
            auto reranked = rerank_client_->Rerank(req.text(), texts);
            if (!reranked.ok) {
                LOG(WARNING) << "rerank failed, keeping RRF order: " << reranked.error;
                degraded.push_back("rerank");
            } else {
                std::vector<HybridHit> reordered;
                reordered.reserve(result.hits.size());
                std::vector<bool> used(n, false);
                for (const auto& r : reranked.results) {
                    if (r.index < n && !used[r.index]) {
                        used[r.index] = true;
                        reordered.push_back(result.hits[r.index]);
                    }
                }
                for (size_t i = 0; i < result.hits.size(); ++i) {
                    if (i >= n || !used[i]) {
                        reordered.push_back(result.hits[i]);
                    }
                }
                result.hits = std::move(reordered);
            }
        }
    }

    proto::SearchResponse resp;
    for (const auto& hit : result.hits) {
        core::Document doc;
        if (!entry->docs.GetByInternal(hit.docid, &doc)) {
            continue;
        }
        auto* out = resp.add_hits();
        out->set_id(doc.id);
        out->set_score(hit.score);
        ToProtoDocument(doc, out->mutable_document(), /*include_internal=*/false);
    }
    for (const auto& marker : degraded) {
        resp.add_degraded(marker);
    }
    resp.set_took_ms(std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count());
    SendJsonResponse(cntl, resp);
}

void HttpApiService::HandleAnalyze(brpc::Controller* cntl,
                                   const auth::Principal& principal,
                                   const std::string& collection) {
    auto registry = context_->Registry(principal.tenant);
    if (registry == nullptr) {
        SendErrorResponse(cntl, 400, "invalid tenant: " + principal.tenant);
        return;
    }
    auto entry = registry->Find(collection);
    if (entry == nullptr) {
        SendErrorResponse(cntl, 404, "unknown collection: " + collection);
        return;
    }
    proto::AnalyzeRequest req;
    if (!ParseJsonBody(cntl, &req)) {
        return;
    }
    proto::AnalyzeResponse resp;
    for (const auto& token : entry->QueryAnalyzer().Analyze(req.text())) {
        auto* out = resp.add_tokens();
        out->set_term(token.term);
        out->set_pos(token.pos);
        out->set_begin(token.begin);
        out->set_end(token.end);
    }
    SendJsonResponse(cntl, resp);
}

// ---------------------------------------------------------------------------
// console session：账号密码登录与自服务
// ---------------------------------------------------------------------------

void HttpApiService::HandleLogin(brpc::Controller* cntl) {
    if (console_auth_ == nullptr || !console_auth_->enabled()) {
        SendErrorResponse(cntl, 404, "console login not enabled");
        return;
    }
    proto::LoginRequest req;
    if (!ParseJsonBody(cntl, &req)) {
        return;
    }
    auto token = console_auth_->Login(req.user(), req.password());
    if (!token.has_value()) {
        SendErrorResponse(cntl, 401, "invalid user or password");
        return;
    }
    proto::LoginResponse resp;
    resp.set_ok(true);
    resp.set_token(*token);
    resp.set_role("admin");
    resp.set_tenant("default");
    resp.set_expires_in(console_auth_->token_ttl_seconds());
    SendJsonResponse(cntl, resp);
}

void HttpApiService::HandleChangePassword(brpc::Controller* cntl) {
    if (console_auth_ == nullptr || !console_auth_->enabled()) {
        SendErrorResponse(cntl, 404, "console login not enabled");
        return;
    }
    proto::ChangePasswordRequest req;
    if (!ParseJsonBody(cntl, &req)) {
        return;
    }
    proto::GenericResponse resp;
    resp.set_ok(console_auth_->ChangePassword(req.user(), req.old_password(), req.new_password()));
    if (!resp.ok()) {
        resp.set_error("invalid credentials or new password too short (>= 6)");
    }
    SendJsonResponse(cntl, resp, resp.ok() ? 200 : 400);
}

void HttpApiService::HandleLogout(brpc::Controller* cntl) {
    if (console_auth_ != nullptr) {
        const std::string* header = cntl->http_request().GetHeader("Authorization");
        const std::string prefix = "Bearer ";
        if (header != nullptr && header->rfind(prefix, 0) == 0) {
            console_auth_->Logout(header->substr(prefix.size()));
        }
    }
    proto::GenericResponse resp;
    resp.set_ok(true);
    SendJsonResponse(cntl, resp);
}

void HttpApiService::HandleWhoAmI(brpc::Controller* cntl, const auth::Principal& principal) {
    proto::WhoAmIResponse resp;
    resp.set_tenant(principal.tenant);
    resp.set_role(auth::RoleToString(principal.role));
    resp.set_key_id(principal.key_id);
    if (principal.key_id == "anonymous") {
        resp.set_auth_type("anonymous");
    } else if (principal.key_id.rfind("session:", 0) == 0) {
        resp.set_auth_type("session");
    } else {
        resp.set_auth_type("api_key");
    }
    SendJsonResponse(cntl, resp);
}

// ---------------------------------------------------------------------------
// admin：租户与 key（DESIGN.md §10.5）
// ---------------------------------------------------------------------------

void HttpApiService::HandleCreateTenant(brpc::Controller* cntl, const auth::Principal& principal) {
    if (!is_admin(principal)) {
        return SendErrorResponse(cntl, 403, "admin role required");
    }
    proto::AdminCreateTenantRequest req;
    if (!ParseJsonBody(cntl, &req)) {
        return;
    }
    if (!core::IsValidResourceName(req.name())) {
        SendErrorResponse(cntl, 400, "tenant name must match [A-Za-z0-9_-]{1,64}");
        return;
    }
    if (context_->HasTenant(req.name())) {
        SendErrorResponse(cntl, 409, "tenant exists: " + req.name());
        return;
    }
    if (context_->Registry(req.name()) == nullptr) {
        SendErrorResponse(cntl, 400, "invalid tenant: " + req.name());
        return;
    }
    proto::GenericResponse resp;
    resp.set_ok(true);
    SendJsonResponse(cntl, resp);
}
void HttpApiService::HandleListTenants(brpc::Controller* cntl, const auth::Principal& principal) {
    if (!is_admin(principal)) {
        return SendErrorResponse(cntl, 403, "admin role required");
    }
    proto::AdminListTenantsResponse resp;
    for (const auto& tenant : context_->Tenants()) {
        auto* entry = resp.add_tenants();
        entry->set_name(tenant.name);
        entry->set_collections(static_cast<int64_t>(tenant.collections));
    }
    SendJsonResponse(cntl, resp);
}

void HttpApiService::HandleDropTenant(brpc::Controller* cntl,
                                      const auth::Principal& principal,
                                      const std::string& name) {
    if (!is_admin(principal)) {
        return SendErrorResponse(cntl, 403, "admin role required");
    }
    const std::string* confirm = cntl->http_request().uri().GetQuery("confirm");
    if (confirm == nullptr || *confirm != name) {
        return SendErrorResponse(cntl, 400, "confirm query parameter must equal the tenant name");
    }
    proto::GenericResponse resp;
    resp.set_ok(context_->DropTenant(name));
    SendJsonResponse(cntl, resp, resp.ok() ? 200 : 404);
}

void HttpApiService::HandleMoveCollection(brpc::Controller* cntl,
                                          const auth::Principal& principal,
                                          const std::string& src_tenant,
                                          const std::string& collection) {
    if (!is_admin(principal)) {
        return SendErrorResponse(cntl, 403, "admin role required");
    }
    proto::MoveCollectionRequest req;
    if (!ParseJsonBody(cntl, &req)) {
        return;
    }
    if (!core::IsValidResourceName(collection) || !core::IsValidResourceName(req.target())) {
        return SendErrorResponse(cntl, 400, "invalid collection or target tenant name");
    }
    auto result = context_->MoveCollection(src_tenant, req.target(), collection);
    proto::MoveCollectionResponse resp;
    resp.set_ok(result.ok);
    resp.set_error(result.error);
    resp.set_documents(static_cast<int64_t>(result.documents));
    // 目标侧已有同名 collection → 409；其他失败（源不存在等）→ 400。
    const bool conflict = !result.ok && result.error.find("target:") == 0;
    SendJsonResponse(cntl, resp, result.ok ? 200 : (conflict ? 409 : 400));
}

void HttpApiService::HandleIssueKey(brpc::Controller* cntl,
                                    const auth::Principal& principal,
                                    const std::string& tenant) {
    const bool allowed = is_admin(principal) ||
                         (principal.role == auth::Role::kTenantAdmin && principal.tenant == tenant);
    if (!allowed) {
        return SendErrorResponse(cntl, 403, "admin or same-tenant tenant_admin required");
    }
    proto::AdminIssueKeyRequest req;
    if (!ParseJsonBody(cntl, &req)) {
        return;
    }
    auto role = auth::RoleFromString(req.role());
    if (!role.has_value() || role == auth::Role::kAdmin) {
        return SendErrorResponse(cntl, 400, "role must be tenant_admin|writer|reader");
    }
    // tenant_admin 只能签发 writer/reader（DESIGN.md §10.2）；签发新的
    // tenant_admin 是 admin 的特权。
    if (principal.role == auth::Role::kTenantAdmin && role == auth::Role::kTenantAdmin) {
        return SendErrorResponse(cntl, 400, "tenant_admin may only issue writer|reader keys");
    }
    std::vector<std::string> collections(req.collections().begin(), req.collections().end());
    auto issued = keys_->Issue(tenant, *role, std::move(collections));
    if (issued.key.empty()) {
        return SendErrorResponse(cntl, 400, "invalid tenant or collection name");
    }
    proto::AdminIssueKeyResponse resp;
    resp.set_key_id(issued.key_id);
    resp.set_key(issued.key); // 明文仅此一次
    SendJsonResponse(cntl, resp);
}

void HttpApiService::HandleListKeys(brpc::Controller* cntl,
                                    const auth::Principal& principal,
                                    const std::string& tenant) {
    const bool allowed = is_admin(principal) ||
                         (principal.role == auth::Role::kTenantAdmin && principal.tenant == tenant);
    if (!allowed) {
        return SendErrorResponse(cntl, 403, "admin or same-tenant tenant_admin required");
    }
    proto::AdminListKeysResponse resp;
    for (const auto& entry : keys_->List()) {
        if (entry.principal.tenant != tenant) {
            continue;
        }
        auto* out = resp.add_keys();
        out->set_key_id(entry.key_id);
        out->set_tenant(entry.principal.tenant);
        out->set_role(auth::RoleToString(entry.principal.role));
        for (const auto& c : entry.principal.collections) {
            out->add_collections(c);
        }
        out->set_created_at(entry.created_at);
        out->set_revoked(entry.revoked);
    }
    SendJsonResponse(cntl, resp);
}

void HttpApiService::HandleRevokeKey(brpc::Controller* cntl,
                                     const auth::Principal& principal,
                                     const std::string& tenant,
                                     const std::string& key_id) {
    const bool allowed = is_admin(principal) ||
                         (principal.role == auth::Role::kTenantAdmin && principal.tenant == tenant);
    if (!allowed) {
        return SendErrorResponse(cntl, 403, "admin or same-tenant tenant_admin required");
    }
    for (const auto& entry : keys_->List()) {
        if (entry.key_id == key_id && entry.principal.tenant != tenant) {
            return SendErrorResponse(cntl, 404, "key belongs to another tenant");
        }
    }
    proto::GenericResponse resp;
    resp.set_ok(keys_->Revoke(key_id));
    SendJsonResponse(cntl, resp, resp.ok() ? 200 : 404);
}

void HttpApiService::HandleMoveKey(brpc::Controller* cntl,
                                   const auth::Principal& principal,
                                   const std::string& src_tenant,
                                   const std::string& key_id) {
    if (!is_admin(principal)) {
        return SendErrorResponse(cntl, 403, "admin role required");
    }
    proto::MoveKeyRequest req;
    if (!ParseJsonBody(cntl, &req)) {
        return;
    }
    if (!core::IsValidResourceName(req.target())) {
        return SendErrorResponse(cntl, 400, "invalid target tenant name");
    }
    if (req.target() == src_tenant) {
        return SendErrorResponse(cntl, 400, "source and target tenant are the same");
    }
    // 校验 key 存在且当前归属 src 租户，避免凭 key_id 猜测跨租户改绑
    bool found = false;
    for (const auto& entry : keys_->List()) {
        if (entry.key_id == key_id) {
            found = true;
            if (entry.principal.tenant != src_tenant) {
                return SendErrorResponse(cntl, 400, "key does not belong to tenant " + src_tenant);
            }
            if (entry.revoked) {
                return SendErrorResponse(cntl, 400, "key already revoked");
            }
            break;
        }
    }
    if (!found) {
        return SendErrorResponse(cntl, 404, "unknown key: " + key_id);
    }
    proto::GenericResponse resp;
    resp.set_ok(keys_->MoveKey(key_id, req.target()));
    SendJsonResponse(cntl, resp, resp.ok() ? 200 : 400);
}

void HttpApiService::HandleStats(brpc::Controller* cntl, const auth::Principal& principal) {
    if (!is_admin(principal)) {
        return SendErrorResponse(cntl, 403, "admin role required");
    }
    proto::AdminStatsResponse resp;
    for (const auto& tenant : context_->Tenants()) {
        auto* out = resp.add_tenants();
        out->set_name(tenant.name);
        out->set_collections(static_cast<int64_t>(tenant.collections));
        out->set_active_documents(static_cast<int64_t>(tenant.active_documents));
        out->set_documents(static_cast<int64_t>(tenant.top_level_documents));
        resp.set_total_collections(resp.total_collections() +
                                   static_cast<int64_t>(tenant.collections));
        resp.set_total_active_documents(resp.total_active_documents() +
                                        static_cast<int64_t>(tenant.active_documents));
        resp.set_total_documents(resp.total_documents() +
                                 static_cast<int64_t>(tenant.top_level_documents));
    }
    SendJsonResponse(cntl, resp);
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

template <typename T> bool HttpApiService::ParseJsonBody(brpc::Controller* cntl, T* message) {
    const std::string body = cntl->request_attachment().to_string();
    std::string error;
    if (!json2pb::JsonToProtoMessage(body, message, &error)) {
        SendErrorResponse(cntl, 400, "invalid JSON: " + error);
        return false;
    }
    return true;
}

template <typename T>
void HttpApiService::SendJsonResponse(brpc::Controller* cntl, const T& message, int status_code) {
    cntl->http_response().set_status_code(status_code);
    cntl->http_response().set_content_type("application/json");
    std::string json;
    json2pb::Pb2JsonOptions options;
    options.always_print_primitive_fields = true;
    json2pb::ProtoMessageToJson(message, &json, options);
    cntl->response_attachment().append(json);
}

void HttpApiService::SendErrorResponse(brpc::Controller* cntl,
                                       int status_code,
                                       const std::string& error) {
    // 经 json2pb 序列化，避免手工拼接时 error 里的引号/换行破坏 JSON。
    proto::GenericResponse resp;
    resp.set_ok(false);
    resp.set_error(error);
    SendJsonResponse(cntl, resp, status_code);
}

} // namespace pl::minisearch::server
