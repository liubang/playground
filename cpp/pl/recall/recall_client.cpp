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

// HTTP/JSON 客户端示例
// 使用 brpc::Channel 发送 HTTP 请求到 VectorRecallService

#include <brpc/channel.h>
#include <brpc/controller.h>
#include <butil/logging.h>
#include <gflags/gflags.h>
#include <random>
#include <sstream>
#include <string>

DEFINE_string(server, "http://127.0.0.1:8200", "Server address (HTTP)");
DEFINE_int32(timeout_ms, 3000, "RPC timeout in milliseconds");
DEFINE_int32(max_retry, 3, "Max retry count");
DEFINE_int32(dimension, 768, "Embedding vector dimension");

namespace {

// 生成随机 embedding 用于演示
std::vector<float> random_embedding(int dim) {
    static std::mt19937 gen(42);
    static std::normal_distribution<float> dist(0.0f, 1.0f);
    std::vector<float> vec(dim);
    for (auto& v : vec) {
        v = dist(gen);
    }
    return vec;
}

// 将 float 数组序列化为 JSON 数组字符串
std::string embedding_to_json_array(const std::vector<float>& emb) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < emb.size(); ++i) {
        if (i > 0)
            oss << ",";
        oss << emb[i];
    }
    oss << "]";
    return oss.str();
}

// 发送 HTTP POST 请求
std::string http_post(brpc::Channel& channel, const std::string& path, const std::string& body) {
    brpc::Controller cntl;
    cntl.http_request().uri() = path;
    cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
    cntl.http_request().set_content_type("application/json");
    cntl.request_attachment().append(body);

    channel.CallMethod(nullptr, &cntl, nullptr, nullptr, nullptr);
    if (cntl.Failed()) {
        LOG(ERROR) << "POST " << path << " failed: " << cntl.ErrorText();
        return "";
    }
    return cntl.response_attachment().to_string();
}

// 发送 HTTP GET 请求
std::string http_get(brpc::Channel& channel, const std::string& path) {
    brpc::Controller cntl;
    cntl.http_request().uri() = path;
    cntl.http_request().set_method(brpc::HTTP_METHOD_GET);

    channel.CallMethod(nullptr, &cntl, nullptr, nullptr, nullptr);
    if (cntl.Failed()) {
        LOG(ERROR) << "GET " << path << " failed: " << cntl.ErrorText();
        return "";
    }
    return cntl.response_attachment().to_string();
}

} // namespace

int main(int argc, char* argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    // 初始化 HTTP channel
    brpc::Channel channel;
    brpc::ChannelOptions options;
    options.protocol = brpc::PROTOCOL_HTTP;
    options.timeout_ms = FLAGS_timeout_ms;
    options.max_retry = FLAGS_max_retry;

    if (channel.Init(FLAGS_server.c_str(), &options) != 0) {
        LOG(ERROR) << "Failed to initialize channel to " << FLAGS_server;
        return -1;
    }

    LOG(INFO) << "Connected to " << FLAGS_server;

    // ========================================================================
    // 1. 逐条添加库表向量
    // ========================================================================
    const std::vector<std::string> table_ids = {
        "default.user_info",      "default.order_detail",      "default.product_catalog",
        "default.payment_record", "default.delivery_tracking",
    };

    for (const auto& table_id : table_ids) {
        auto emb = random_embedding(FLAGS_dimension);
        auto emb_json = embedding_to_json_array(emb);

        // 简单解析 database.table
        std::string database, table;
        auto dot = table_id.find('.');
        if (dot != std::string::npos) {
            database = table_id.substr(0, dot);
            table = table_id.substr(dot + 1);
        }

        std::ostringstream body;
        body << "{"
             << "\"table_id\":\"" << table_id << "\","
             << "\"embedding\":" << emb_json << ","
             << "\"meta\":{"
             << "\"database\":\"" << database << "\","
             << "\"table\":\"" << table << "\","
             << "\"comment\":\"demo table: " << table_id << "\""
             << "}"
             << "}";

        auto resp = http_post(channel, "/api/recall/add", body.str());
        LOG(INFO) << "Add " << table_id << " => " << resp;
    }

    // ========================================================================
    // 2. 批量添加示例
    // ========================================================================
    {
        auto emb1 = random_embedding(FLAGS_dimension);
        auto emb2 = random_embedding(FLAGS_dimension);

        std::ostringstream body;
        body << "{\"items\":["
             << "{\"table_id\":\"analytics.daily_report\","
             << "\"embedding\":" << embedding_to_json_array(emb1) << ","
             << "\"meta\":{\"database\":\"analytics\",\"table\":\"daily_report\","
             << "\"comment\":\"batch item 1\"}},"
             << "{\"table_id\":\"analytics.weekly_summary\","
             << "\"embedding\":" << embedding_to_json_array(emb2) << ","
             << "\"meta\":{\"database\":\"analytics\",\"table\":\"weekly_summary\","
             << "\"comment\":\"batch item 2\"}}"
             << "]}";

        auto resp = http_post(channel, "/api/recall/batch_add", body.str());
        LOG(INFO) << "BatchAdd => " << resp;
    }

    // ========================================================================
    // 3. 查询索引状态
    // ========================================================================
    {
        auto resp = http_get(channel, "/api/recall/stats");
        LOG(INFO) << "Stats => " << resp;
    }

    // ========================================================================
    // 4. 向量检索
    // ========================================================================
    {
        auto query = random_embedding(FLAGS_dimension);
        std::ostringstream body;
        body << "{"
             << "\"embedding\":" << embedding_to_json_array(query) << ","
             << "\"top_k\":3"
             << "}";

        auto resp = http_post(channel, "/api/recall/search", body.str());
        LOG(INFO) << "Search => " << resp;
    }

    // ========================================================================
    // 5. 保存快照
    // ========================================================================
    {
        std::string body = R"({"path":"/tmp/recall_snapshot"})";
        auto resp = http_post(channel, "/api/recall/snapshot/save", body);
        LOG(INFO) << "SaveSnapshot => " << resp;
    }

    // ========================================================================
    // 6. 加载快照
    // ========================================================================
    {
        std::string body = R"({"path":"/tmp/recall_snapshot"})";
        auto resp = http_post(channel, "/api/recall/snapshot/load", body);
        LOG(INFO) << "LoadSnapshot => " << resp;
    }

    LOG(INFO) << "All done.";
    return 0;
}
