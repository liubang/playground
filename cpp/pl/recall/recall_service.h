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

#pragma once

#include "cpp/pl/recall/embedding_client.h"
#include "cpp/pl/recall/faiss_index.h"
#include "cpp/pl/recall/proto/recall.pb.h"
#include <brpc/controller.h>
#include <mutex>
#include <string>
#include <unordered_map>

namespace pl::recall {

// 库表元信息缓存，与向量索引配合使用
class MetaStore {
public:
    void put(const std::string& table_id, const proto::TableMeta& meta);
    bool get(const std::string& table_id, proto::TableMeta* meta) const;
    int64_t size() const;

    // 持久化
    bool save(const std::string& path) const;
    bool load(const std::string& path);

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, proto::TableMeta> store_;
};

// HTTP/JSON 服务实现
//
// 继承自 proto 生成的 RecallHttpService，通过 default_method 接收所有 HTTP 请求，
// 根据 URL path 分发到不同的处理逻辑。
//
// RESTful API:
//   POST /api/recall/add              添加单条库表向量
//   POST /api/recall/batch_add        批量添加
//   POST /api/recall/search           向量检索
//   POST /api/recall/add_by_text      文本添加（自动计算 embedding）
//   POST /api/recall/batch_add_by_text 批量文本添加
//   POST /api/recall/search_by_text   文本检索（自动计算 query embedding）
//   POST /api/recall/snapshot/save    保存快照
//   POST /api/recall/snapshot/load    加载快照
//   GET  /api/recall/stats            查询索引状态
class RecallHttpServiceImpl : public proto::RecallHttpService {
public:
    // embedding_client 可为 nullptr，此时 *_by_text 接口返回 503
    RecallHttpServiceImpl(int dimension,
                          const std::string& index_type,
                          std::shared_ptr<EmbeddingClient> embedding_client = nullptr);
    ~RecallHttpServiceImpl() override = default;

    // 启动时加载快照，在 AddService 之前调用
    bool Init(const std::string& snapshot_path);

    // brpc HTTP 入口：所有 /api/recall/* 请求都路由到这里
    void default_method(google::protobuf::RpcController* controller,
                        const proto::HttpRequest* request,
                        proto::HttpResponse* response,
                        google::protobuf::Closure* done) override;

private:
    void HandleAdd(brpc::Controller* cntl);
    void HandleBatchAdd(brpc::Controller* cntl);
    void HandleSearch(brpc::Controller* cntl);
    void HandleAddByText(brpc::Controller* cntl);
    void HandleBatchAddByText(brpc::Controller* cntl);
    void HandleSearchByText(brpc::Controller* cntl);
    void HandleSaveSnapshot(brpc::Controller* cntl);
    void HandleLoadSnapshot(brpc::Controller* cntl);
    void HandleGetStats(brpc::Controller* cntl);

    // 工具方法：从 request body 解析 JSON 到 protobuf
    template <typename T> bool ParseJsonBody(brpc::Controller* cntl, T* message);

    // 工具方法：将 protobuf 序列化为 JSON 写入 response body
    template <typename T>
    void SendJsonResponse(brpc::Controller* cntl, const T& message, int status_code = 200);

    // 写入错误 JSON 响应
    void SendErrorResponse(brpc::Controller* cntl, int status_code, const std::string& error);

    FaissIndex index_;
    IdMapper id_mapper_;
    MetaStore meta_store_;
    std::shared_ptr<EmbeddingClient> embedding_client_;
};

} // namespace pl::recall
