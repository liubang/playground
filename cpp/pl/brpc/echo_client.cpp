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
// Created: 2026/05/06 21:10

#include <brpc/channel.h>
#include <gflags/gflags.h>

#include "cpp/pl/brpc/proto/echo.pb.h"

DEFINE_string(server, "0.0.0.0:8000", "IP Address of server");
DEFINE_string(load_balancer, "", "The algorithm for load balancing");
DEFINE_int32(timeout_ms, 100, "RPC timeout in milliseconds");
DEFINE_int32(max_retry, 3, "Max retries(not including the first RPC)");
DEFINE_int32(request_count, 10, "Number of requests to send");

int main(int argc, char* argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    ::brpc::Channel channel;

    ::brpc::ChannelOptions options;
    options.timeout_ms = FLAGS_timeout_ms;
    options.max_retry = FLAGS_max_retry;

    // 初始化 channel，连接到 server
    if (channel.Init(FLAGS_server.c_str(), FLAGS_load_balancer.c_str(), &options) != 0) {
        LOG(ERROR) << "Fail to initialize channel";
        return -1;
    }

    // 通过 channel 访问 EchoService，不需要手动创建连接
    pl::brpc::proto::EchoService_Stub stub(&channel);

    for (int i = 0; i < FLAGS_request_count; ++i) {
        pl::brpc::proto::EchoRequest request;
        pl::brpc::proto::EchoResponse response;
        ::brpc::Controller cntl;

        request.set_message("hello brpc " + std::to_string(i));

        // 同步调用，cntl 中携带了请求的上下文和结果
        stub.Echo(&cntl, &request, &response, nullptr);

        if (!cntl.Failed()) {
            LOG(INFO) << "Received response from " << cntl.remote_side() << " to "
                      << cntl.local_side() << ": " << response.message()
                      << " latency=" << cntl.latency_us() << "us";
        } else {
            LOG(WARNING) << cntl.ErrorText();
        }
    }

    return 0;
}
