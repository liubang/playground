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
// Created: 2026/05/14 10:45

// v2 API 示例客户端：创建 collection、写入文档（client 向量）、检索、读取、删除。
// 使用 brpc HTTP Channel 发送 JSON 请求。

#include <brpc/channel.h>
#include <gflags/gflags.h>
#include <iostream>
#include <string>

DEFINE_string(server, "http://127.0.0.1:8200", "MiniSearch server address");

namespace {

struct Reply {
    int status = 0;
    std::string body;
};

Reply http_request(brpc::Channel& channel,
                   brpc::HttpMethod method,
                   const std::string& path,
                   const std::string& body = "") {
    brpc::Controller cntl;
    cntl.http_request().set_method(method);
    cntl.http_request().uri() = FLAGS_server + path;
    if (!body.empty()) {
        cntl.request_attachment().append(body);
    }
    channel.CallMethod(nullptr, &cntl, nullptr, nullptr, nullptr);
    return {cntl.http_response().status_code(), cntl.response_attachment().to_string()};
}

} // namespace

int main(int argc, char* argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    brpc::Channel channel;
    brpc::ChannelOptions options;
    options.protocol = "http";
    if (channel.Init(FLAGS_server.substr(FLAGS_server.find("://") + 3).c_str(), "", &options) !=
        0) {
        std::cerr << "Failed to init channel to " << FLAGS_server << std::endl;
        return -1;
    }

    // 1. 创建 collection（title text / tags keyword / vec vector(4, client 模式）
    const std::string schema =
        R"({"name":"demo","default_analyzer":"cjk_jieba","fields":[)"
        R"({"name":"title","type":"text","indexed":true,"stored":true},)"
        R"({"name":"tags","type":"keyword","indexed":true,"stored":true},)"
        R"({"name":"vec","type":"vector","indexed":false,"stored":true,"dims":4,)"
        R"("metric":"cosine","mode":"client"}]})";
    auto reply = http_request(channel, brpc::HTTP_METHOD_POST, "/api/v2/collections", schema);
    std::cout << "[create] " << reply.status << " " << reply.body << std::endl;

    // 2. 写入两条文档
    const std::string doc1 =
        R"({"version":1,"fields":{"title":{"s":"presto 调优"},"tags":{"s":"wiki"},)"
        R"("vec":{"v":{"data":[1.0,0.0,0.0,0.0]}}}})";
    const std::string doc2 =
        R"({"version":1,"fields":{"title":{"s":"loom 架构"},"tags":{"s":"wiki"},)"
        R"("vec":{"v":{"data":[0.0,1.0,0.0,0.0]}}}})";
    reply = http_request(channel, brpc::HTTP_METHOD_PUT, "/api/v2/demo/documents/doc1", doc1);
    std::cout << "[upsert doc1] " << reply.status << " " << reply.body << std::endl;
    reply = http_request(channel, brpc::HTTP_METHOD_PUT, "/api/v2/demo/documents/doc2", doc2);
    std::cout << "[upsert doc2] " << reply.status << " " << reply.body << std::endl;

    // 3. 向量检索（靠近 doc1 的 query）
    reply = http_request(channel,
                         brpc::HTTP_METHOD_POST,
                         "/api/v2/demo/search",
                         R"({"embedding":[0.9,0.1,0.0,0.0],"top_k":1})");
    std::cout << "[search] " << reply.status << " " << reply.body << std::endl;

    // 4. 读取、删除
    reply = http_request(channel, brpc::HTTP_METHOD_GET, "/api/v2/demo/documents/doc1");
    std::cout << "[get doc1] " << reply.status << " " << reply.body << std::endl;
    reply = http_request(channel, brpc::HTTP_METHOD_DELETE, "/api/v2/demo/documents/doc2");
    std::cout << "[delete doc2] " << reply.status << " " << reply.body << std::endl;

    // 5. 删除 collection（需 confirm）
    reply =
        http_request(channel, brpc::HTTP_METHOD_DELETE, "/api/v2/collections/demo?confirm=demo");
    std::cout << "[drop] " << reply.status << " " << reply.body << std::endl;
    return 0;
}
