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
#include <filesystem>
#include <fstream>
#include <json2pb/json_to_pb.h>
#include <json2pb/pb_to_json.h>
#include <unordered_set>

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

bool MetaStore::save(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) {
        return false;
    }
    // 格式: [count][table_id_len, table_id, serialized_meta_len, serialized_meta]...
    auto count = static_cast<int64_t>(store_.size());
    ofs.write(reinterpret_cast<const char*>(&count), sizeof(count));
    for (const auto& [table_id, meta] : store_) {
        auto tid_len = static_cast<int32_t>(table_id.size());
        ofs.write(reinterpret_cast<const char*>(&tid_len), sizeof(tid_len));
        ofs.write(table_id.data(), tid_len);

        std::string serialized;
        meta.SerializeToString(&serialized);
        auto meta_len = static_cast<int32_t>(serialized.size());
        ofs.write(reinterpret_cast<const char*>(&meta_len), sizeof(meta_len));
        ofs.write(serialized.data(), meta_len);
    }
    return ofs.good();
}

bool MetaStore::load(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    store_.clear();

    int64_t count = 0;
    ifs.read(reinterpret_cast<char*>(&count), sizeof(count));
    for (int64_t i = 0; i < count; ++i) {
        int32_t tid_len = 0;
        ifs.read(reinterpret_cast<char*>(&tid_len), sizeof(tid_len));
        std::string table_id(static_cast<size_t>(tid_len), '\0');
        ifs.read(table_id.data(), tid_len);

        int32_t meta_len = 0;
        ifs.read(reinterpret_cast<char*>(&meta_len), sizeof(meta_len));
        std::string serialized(static_cast<size_t>(meta_len), '\0');
        ifs.read(serialized.data(), meta_len);

        proto::TableMeta meta;
        meta.ParseFromString(serialized);
        store_[std::move(table_id)] = std::move(meta);
    }
    return ifs.good();
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
    cntl->response_attachment().append(R"({"error":")" + error + R"("})");
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

    // 确保目录存在
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        resp.set_success(false);
        resp.set_message("failed to create directory: " + ec.message());
        SendJsonResponse(cntl, resp, 500);
        return;
    }

    std::string index_path = path + "/faiss.index";
    std::string mapper_path = path + "/id_mapper.bin";
    std::string meta_path = path + "/meta_store.bin";

    bool ok =
        index_.save(index_path) && id_mapper_.save(mapper_path) && meta_store_.save(meta_path);
    resp.set_success(ok);
    if (ok) {
        resp.set_message("snapshot saved to " + path +
                         " (vectors=" + std::to_string(index_.size()) +
                         ", tables=" + std::to_string(meta_store_.size()) + ")");
        LOG(INFO) << "Snapshot saved to " << path << " vectors=" << index_.size()
                  << " tables=" << meta_store_.size();
    } else {
        resp.set_message("failed to save snapshot to " + path);
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
    std::string meta_path = path + "/meta_store.bin";

    bool ok = index_.load(index_path) && id_mapper_.load(mapper_path);
    // meta_store 加载失败不致命，只是丢失元信息
    if (ok) {
        if (!meta_store_.load(meta_path)) {
            LOG(WARNING) << "MetaStore load failed (non-fatal), meta_path=" << meta_path;
        }
    }
    resp.set_success(ok);
    if (ok) {
        resp.set_message("snapshot loaded from " + path +
                         " (vectors=" + std::to_string(index_.size()) +
                         ", tables=" + std::to_string(meta_store_.size()) + ")");
        LOG(INFO) << "Snapshot loaded from " << path << " vectors=" << index_.size()
                  << " tables=" << meta_store_.size();
    } else {
        resp.set_message("failed to load snapshot from " + path);
        LOG(WARNING) << "Failed to load snapshot from " << path;
    }
    SendJsonResponse(cntl, resp, ok ? 200 : 500);
}

// =========================================================================
// Text-based Handlers (auto-embedding)
// =========================================================================

// 将一条记录拆分为多个语义视角的文本，分别 embed 入库。
// 这样短 query（如 "测试表"）能匹配到 comment 视角的向量。
static std::vector<std::string> SplitTextViews(const std::string& full_text,
                                               const proto::TableMeta& meta) {
    std::vector<std::string> views;
    // 视角 1: 完整文本（原始行为）
    views.push_back(full_text);
    // 视角 2: 纯 comment（如果非空且与 full_text 不同）
    if (!meta.comment().empty() && meta.comment() != full_text) {
        views.push_back(meta.comment());
    }
    return views;
}

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

    // 拆分多视角文本
    proto::TableMeta meta;
    if (req.has_meta()) {
        meta = req.meta();
    }
    auto views = SplitTextViews(req.text(), meta);

    // 批量 embed 所有视角
    auto emb_result = embedding_client_->EmbedBatch(views);
    if (!emb_result.ok) {
        SendErrorResponse(cntl, 502, "embedding failed: " + emb_result.error);
        return;
    }

    int added = 0;
    for (size_t i = 0; i < views.size(); ++i) {
        const auto& emb = emb_result.embeddings[i];
        if (static_cast<int>(emb.size()) != index_.dimension()) {
            continue;
        }
        int64_t numeric_id = id_mapper_.assign_new(req.table_id());
        if (index_.add(numeric_id, emb.data())) {
            ++added;
        }
    }

    if (req.has_meta()) {
        meta_store_.put(req.table_id(), req.meta());
    }

    proto::AddResponse resp;
    resp.set_success(added > 0);
    if (added == 0) {
        resp.set_message("failed to add any vector to index");
        SendJsonResponse(cntl, resp, 500);
    } else {
        SendJsonResponse(cntl, resp);
    }
    LOG(INFO) << "AddByText table_id=" << req.table_id() << " views=" << views.size()
              << " added=" << added << " from " << cntl->remote_side();
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

    // 第一步：为每个 item 拆分多视角文本，并收集所有文本用于批量 embed
    struct ViewInfo {
        int item_idx;     // 对应哪个原始 item
        std::string text; // 视角文本
    };
    std::vector<ViewInfo> all_views;
    std::vector<std::string> all_texts;

    for (int i = 0; i < req.items_size(); ++i) {
        const auto& item = req.items(i);
        proto::TableMeta meta;
        if (item.has_meta()) {
            meta = item.meta();
        }
        auto views = SplitTextViews(item.text(), meta);
        for (auto& v : views) {
            all_views.push_back({.item_idx = i, .text = v});
            all_texts.push_back(std::move(v));
        }
    }

    // 第二步：一次性批量 embed 所有视角文本
    auto emb_result = embedding_client_->EmbedBatch(all_texts);
    if (!emb_result.ok) {
        SendErrorResponse(cntl, 502, "batch embedding failed: " + emb_result.error);
        return;
    }

    // 第三步：入库
    int success_count = 0;
    int fail_count = 0;
    // 跟踪每个 item 是否至少有一个视角成功
    std::vector<bool> item_ok(req.items_size(), false);

    for (size_t vi = 0; vi < all_views.size(); ++vi) {
        int item_idx = all_views[vi].item_idx;
        const auto& item = req.items(item_idx);
        const auto& emb = emb_result.embeddings[vi];

        if (item.table_id().empty() || static_cast<int>(emb.size()) != index_.dimension()) {
            continue;
        }

        int64_t numeric_id = id_mapper_.assign_new(item.table_id());
        if (index_.add(numeric_id, emb.data())) {
            if (!item_ok[item_idx]) {
                item_ok[item_idx] = true;
                // meta 只存一次
                if (item.has_meta()) {
                    meta_store_.put(item.table_id(), item.meta());
                }
            }
        }
    }

    for (int i = 0; i < req.items_size(); ++i) {
        if (item_ok[i])
            ++success_count;
        else
            ++fail_count;
    }

    proto::BatchAddResponse resp;
    resp.set_success_count(success_count);
    resp.set_fail_count(fail_count);
    SendJsonResponse(cntl, resp);
    LOG(INFO) << "BatchAddByText items=" << req.items_size() << " views=" << all_views.size()
              << " success=" << success_count << " fail=" << fail_count << " from "
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
    // 因为多视角入库，同一个 table_id 可能占多个槽位，所以多取一些再去重
    int search_k = top_k * 3;
    auto results = index_.search(emb_result.embedding.data(), search_k);

    // 按 table_id 去重，保留每个 table_id 的最小 distance
    proto::SearchResponse resp;
    std::unordered_set<std::string> seen;
    for (const auto& r : results) {
        std::string table_id = id_mapper_.get_table_id(r.id);
        if (table_id.empty() || (seen.count(table_id) != 0u)) {
            continue;
        }
        seen.insert(table_id);

        auto* item = resp.add_results();
        item->set_table_id(table_id);
        item->set_distance(r.distance);

        proto::TableMeta meta;
        if (meta_store_.get(table_id, &meta)) {
            *item->mutable_meta() = std::move(meta);
        }

        if (resp.results_size() >= top_k) {
            break;
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
