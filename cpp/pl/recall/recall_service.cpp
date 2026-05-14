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

#include "cpp/pl/recall/recall_service.h"

#include <brpc/closure_guard.h>
#include <brpc/controller.h>
#include <json2pb/json_to_pb.h>
#include <json2pb/pb_to_json.h>

namespace pl::recall {

// =========================================================================
// MetaStore
// =========================================================================

void MetaStore::put(const std::string& table_id, const proto::TableMeta& meta) {
    std::lock_guard<std::mutex> lock(mu_);
    store_[table_id] = meta;
}

bool MetaStore::get(const std::string& table_id, proto::TableMeta* meta) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = store_.find(table_id);
    if (it == store_.end()) {
        return false;
    }
    *meta = it->second;
    return true;
}

int64_t MetaStore::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return static_cast<int64_t>(store_.size());
}

// =========================================================================
// JSON 工具方法
// =========================================================================

template <typename T>
bool RecallHttpServiceImpl::ParseJsonBody(brpc::Controller* cntl, T* message) {
    const std::string body = cntl->request_attachment().to_string();
    std::string error;
    if (!json2pb::JsonToProtoMessage(body, message, &error)) {
        SendErrorResponse(cntl, 400, "invalid JSON: " + error);
        return false;
    }
    return true;
}

template <typename T>
void RecallHttpServiceImpl::SendJsonResponse(brpc::Controller* cntl,
                                             const T& message,
                                             int status_code) {
    cntl->http_response().set_status_code(status_code);
    cntl->http_response().set_content_type("application/json");

    std::string json;
    json2pb::Pb2JsonOptions options;
    options.always_print_primitive_fields = true;
    json2pb::ProtoMessageToJson(message, &json, options);
    cntl->response_attachment().append(json);
}

void RecallHttpServiceImpl::SendErrorResponse(brpc::Controller* cntl,
                                              int status_code,
                                              const std::string& error) {
    cntl->http_response().set_status_code(status_code);
    cntl->http_response().set_content_type("application/json");
    cntl->response_attachment().append("{\"error\":\"" + error + "\"}");
}

// =========================================================================
// RecallHttpServiceImpl
// =========================================================================

RecallHttpServiceImpl::RecallHttpServiceImpl(int dimension,
                                             const std::string& index_type,
                                             std::shared_ptr<EmbeddingClient> embedding_client)
    : index_(dimension, index_type), embedding_client_(std::move(embedding_client)) {}

bool RecallHttpServiceImpl::Init(const std::string& snapshot_path) {
    if (snapshot_path.empty()) {
        return true;
    }
    std::string index_path = snapshot_path + "/faiss.index";
    std::string mapper_path = snapshot_path + "/id_mapper.bin";
    return index_.load(index_path) && id_mapper_.load(mapper_path);
}

void RecallHttpServiceImpl::default_method(google::protobuf::RpcController* controller_base,
                                           const proto::HttpRequest* /*request*/,
                                           proto::HttpResponse* /*response*/,
                                           google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    auto* cntl = static_cast<brpc::Controller*>(controller_base);

    const std::string& path = cntl->http_request().uri().path();

    if (path == "/api/recall/add") {
        HandleAdd(cntl);
    } else if (path == "/api/recall/batch_add") {
        HandleBatchAdd(cntl);
    } else if (path == "/api/recall/search") {
        HandleSearch(cntl);
    } else if (path == "/api/recall/add_by_text") {
        HandleAddByText(cntl);
    } else if (path == "/api/recall/batch_add_by_text") {
        HandleBatchAddByText(cntl);
    } else if (path == "/api/recall/search_by_text") {
        HandleSearchByText(cntl);
    } else if (path == "/api/recall/snapshot/save") {
        HandleSaveSnapshot(cntl);
    } else if (path == "/api/recall/snapshot/load") {
        HandleLoadSnapshot(cntl);
    } else if (path == "/api/recall/stats") {
        HandleGetStats(cntl);
    } else {
        SendErrorResponse(cntl, 404, "not found: " + path);
    }
}

// =========================================================================
// HTTP Handlers
// =========================================================================

void RecallHttpServiceImpl::HandleAdd(brpc::Controller* cntl) {
    proto::AddRequest req;
    if (!ParseJsonBody(cntl, &req)) {
        return;
    }

    proto::AddResponse resp;
    const auto& table_id = req.table_id();
    if (table_id.empty() || req.embedding_size() != index_.dimension()) {
        resp.set_success(false);
        resp.set_message("invalid table_id or embedding dimension mismatch");
        SendJsonResponse(cntl, resp, 400);
        return;
    }

    int64_t numeric_id = id_mapper_.get_or_assign(table_id);
    bool ok = index_.add(numeric_id, req.embedding().data());
    if (!ok) {
        resp.set_success(false);
        resp.set_message("failed to add vector to index");
        SendJsonResponse(cntl, resp, 500);
        return;
    }

    if (req.has_meta()) {
        meta_store_.put(table_id, req.meta());
    }

    resp.set_success(true);
    SendJsonResponse(cntl, resp);
    LOG(INFO) << "Add table_id=" << table_id << " numeric_id=" << numeric_id << " from "
              << cntl->remote_side();
}

void RecallHttpServiceImpl::HandleBatchAdd(brpc::Controller* cntl) {
    proto::BatchAddRequest req;
    if (!ParseJsonBody(cntl, &req)) {
        return;
    }

    int success_count = 0;
    int fail_count = 0;

    for (const auto& item : req.items()) {
        if (item.table_id().empty() || item.embedding_size() != index_.dimension()) {
            ++fail_count;
            continue;
        }

        int64_t numeric_id = id_mapper_.get_or_assign(item.table_id());
        if (index_.add(numeric_id, item.embedding().data())) {
            if (item.has_meta()) {
                meta_store_.put(item.table_id(), item.meta());
            }
            ++success_count;
        } else {
            ++fail_count;
        }
    }

    proto::BatchAddResponse resp;
    resp.set_success_count(success_count);
    resp.set_fail_count(fail_count);
    SendJsonResponse(cntl, resp);
    LOG(INFO) << "BatchAdd success=" << success_count << " fail=" << fail_count << " from "
              << cntl->remote_side();
}

void RecallHttpServiceImpl::HandleSearch(brpc::Controller* cntl) {
    proto::SearchRequest req;
    if (!ParseJsonBody(cntl, &req)) {
        return;
    }

    if (req.embedding_size() != index_.dimension()) {
        SendErrorResponse(cntl, 400, "embedding dimension mismatch");
        return;
    }

    int top_k = req.top_k() > 0 ? req.top_k() : 10;
    auto results = index_.search(req.embedding().data(), top_k);

    proto::SearchResponse resp;
    for (const auto& r : results) {
        auto* item = resp.add_results();
        std::string table_id = id_mapper_.get_table_id(r.id);
        item->set_table_id(table_id);
        item->set_distance(r.distance);

        proto::TableMeta meta;
        if (meta_store_.get(table_id, &meta)) {
            *item->mutable_meta() = std::move(meta);
        }
    }

    SendJsonResponse(cntl, resp);
}

void RecallHttpServiceImpl::HandleSaveSnapshot(brpc::Controller* cntl) {
    proto::SnapshotRequest req;
    if (!ParseJsonBody(cntl, &req)) {
        return;
    }

    proto::SnapshotResponse resp;
    const auto& path = req.path();
    if (path.empty()) {
        resp.set_success(false);
        resp.set_message("path is empty");
        SendJsonResponse(cntl, resp, 400);
        return;
    }

    std::string index_path = path + "/faiss.index";
    std::string mapper_path = path + "/id_mapper.bin";

    bool ok = index_.save(index_path) && id_mapper_.save(mapper_path);
    resp.set_success(ok);
    if (ok) {
        resp.set_message("snapshot saved to " + path);
        LOG(INFO) << "Snapshot saved to " << path;
    } else {
        resp.set_message("failed to save snapshot");
        LOG(WARNING) << "Failed to save snapshot to " << path;
    }
    SendJsonResponse(cntl, resp, ok ? 200 : 500);
}

void RecallHttpServiceImpl::HandleLoadSnapshot(brpc::Controller* cntl) {
    proto::SnapshotRequest req;
    if (!ParseJsonBody(cntl, &req)) {
        return;
    }

    proto::SnapshotResponse resp;
    const auto& path = req.path();
    if (path.empty()) {
        resp.set_success(false);
        resp.set_message("path is empty");
        SendJsonResponse(cntl, resp, 400);
        return;
    }

    std::string index_path = path + "/faiss.index";
    std::string mapper_path = path + "/id_mapper.bin";

    bool ok = index_.load(index_path) && id_mapper_.load(mapper_path);
    resp.set_success(ok);
    if (ok) {
        resp.set_message("snapshot loaded from " + path);
        LOG(INFO) << "Snapshot loaded from " << path << ", vectors=" << index_.size();
    } else {
        resp.set_message("failed to load snapshot");
        LOG(WARNING) << "Failed to load snapshot from " << path;
    }
    SendJsonResponse(cntl, resp, ok ? 200 : 500);
}

// =========================================================================
// Text-based Handlers (auto-embedding)
// =========================================================================

void RecallHttpServiceImpl::HandleAddByText(brpc::Controller* cntl) {
    if (!embedding_client_) {
        SendErrorResponse(cntl, 503, "embedding service not configured");
        return;
    }

    proto::AddByTextRequest req;
    if (!ParseJsonBody(cntl, &req)) {
        return;
    }

    if (req.table_id().empty() || req.text().empty()) {
        SendErrorResponse(cntl, 400, "table_id and text are required");
        return;
    }

    auto emb_result = embedding_client_->Embed(req.text());
    if (!emb_result.ok) {
        SendErrorResponse(cntl, 502, "embedding failed: " + emb_result.error);
        return;
    }

    if (static_cast<int>(emb_result.embedding.size()) != index_.dimension()) {
        SendErrorResponse(cntl, 500,
                          "embedding dimension mismatch: got " +
                              std::to_string(emb_result.embedding.size()) + " expected " +
                              std::to_string(index_.dimension()));
        return;
    }

    int64_t numeric_id = id_mapper_.get_or_assign(req.table_id());
    bool ok = index_.add(numeric_id, emb_result.embedding.data());

    proto::AddResponse resp;
    if (!ok) {
        resp.set_success(false);
        resp.set_message("failed to add vector to index");
        SendJsonResponse(cntl, resp, 500);
        return;
    }

    if (req.has_meta()) {
        meta_store_.put(req.table_id(), req.meta());
    }

    resp.set_success(true);
    SendJsonResponse(cntl, resp);
    LOG(INFO) << "AddByText table_id=" << req.table_id() << " text_len=" << req.text().size()
              << " from " << cntl->remote_side();
}

void RecallHttpServiceImpl::HandleBatchAddByText(brpc::Controller* cntl) {
    if (!embedding_client_) {
        SendErrorResponse(cntl, 503, "embedding service not configured");
        return;
    }

    proto::BatchAddByTextRequest req;
    if (!ParseJsonBody(cntl, &req)) {
        return;
    }

    // 收集所有文本，一次性批量调用 embedding API
    std::vector<std::string> texts;
    texts.reserve(req.items_size());
    for (const auto& item : req.items()) {
        texts.push_back(item.text());
    }

    auto emb_result = embedding_client_->EmbedBatch(texts);
    if (!emb_result.ok) {
        SendErrorResponse(cntl, 502, "batch embedding failed: " + emb_result.error);
        return;
    }

    int success_count = 0;
    int fail_count = 0;

    for (int i = 0; i < req.items_size(); ++i) {
        const auto& item = req.items(i);
        const auto& emb = emb_result.embeddings[i];

        if (item.table_id().empty() || static_cast<int>(emb.size()) != index_.dimension()) {
            ++fail_count;
            continue;
        }

        int64_t numeric_id = id_mapper_.get_or_assign(item.table_id());
        if (index_.add(numeric_id, emb.data())) {
            if (item.has_meta()) {
                meta_store_.put(item.table_id(), item.meta());
            }
            ++success_count;
        } else {
            ++fail_count;
        }
    }

    proto::BatchAddResponse resp;
    resp.set_success_count(success_count);
    resp.set_fail_count(fail_count);
    SendJsonResponse(cntl, resp);
    LOG(INFO) << "BatchAddByText success=" << success_count << " fail=" << fail_count << " from "
              << cntl->remote_side();
}

void RecallHttpServiceImpl::HandleSearchByText(brpc::Controller* cntl) {
    if (!embedding_client_) {
        SendErrorResponse(cntl, 503, "embedding service not configured");
        return;
    }

    proto::SearchByTextRequest req;
    if (!ParseJsonBody(cntl, &req)) {
        return;
    }

    if (req.text().empty()) {
        SendErrorResponse(cntl, 400, "text is required");
        return;
    }

    auto emb_result = embedding_client_->Embed(req.text());
    if (!emb_result.ok) {
        SendErrorResponse(cntl, 502, "embedding failed: " + emb_result.error);
        return;
    }

    if (static_cast<int>(emb_result.embedding.size()) != index_.dimension()) {
        SendErrorResponse(cntl, 500,
                          "embedding dimension mismatch: got " +
                              std::to_string(emb_result.embedding.size()) + " expected " +
                              std::to_string(index_.dimension()));
        return;
    }

    int top_k = req.top_k() > 0 ? req.top_k() : 10;
    auto results = index_.search(emb_result.embedding.data(), top_k);

    proto::SearchResponse resp;
    for (const auto& r : results) {
        auto* item = resp.add_results();
        std::string table_id = id_mapper_.get_table_id(r.id);
        item->set_table_id(table_id);
        item->set_distance(r.distance);

        proto::TableMeta meta;
        if (meta_store_.get(table_id, &meta)) {
            *item->mutable_meta() = std::move(meta);
        }
    }

    SendJsonResponse(cntl, resp);
    LOG(INFO) << "SearchByText text_len=" << req.text().size() << " top_k=" << top_k
              << " results=" << resp.results_size() << " from " << cntl->remote_side();
}

void RecallHttpServiceImpl::HandleGetStats(brpc::Controller* cntl) {
    proto::StatsResponse resp;
    resp.set_total_vectors(index_.size());
    resp.set_dimension(index_.dimension());
    resp.set_index_type(index_.index_type());
    resp.set_is_trained(index_.is_trained());
    SendJsonResponse(cntl, resp);
}

} // namespace pl::recall
