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
// Created: 2026/06/21

#include <brpc/channel.h>
#include <chrono>
#include <gflags/gflags.h>
#include <string>

#include "cpp/pl/brpc/echo/proto/echo.pb.h"

DEFINE_string(server, "0.0.0.0:8000", "IP Address of server");
DEFINE_string(load_balancer, "", "The algorithm for load balancing");
DEFINE_int32(timeout_ms, 1000, "RPC timeout in milliseconds");
DEFINE_int32(max_retry, 3, "Max retries (not including the first RPC)");

static int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int main(int argc, char* argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    ::brpc::Channel channel;
    ::brpc::ChannelOptions options;
    options.timeout_ms = FLAGS_timeout_ms;
    options.max_retry = FLAGS_max_retry;

    if (channel.Init(FLAGS_server.c_str(), FLAGS_load_balancer.c_str(), &options) != 0) {
        LOG(ERROR) << "Fail to initialize channel";
        return -1;
    }

    ::pl::brpc::echo::EchoService_Stub stub(&channel);

    // --- Unary Echo ---
    {
        ::pl::brpc::echo::EchoRequest request;
        ::pl::brpc::echo::EchoResponse response;
        ::brpc::Controller cntl;

        int64_t sent_us = now_us();
        request.set_message("hello brpc from C++ client");
        request.set_timestamp_us(sent_us);
        (*request.mutable_headers())["client"] = "cpp-brpc";

        stub.Echo(&cntl, &request, &response, nullptr);

        if (!cntl.Failed()) {
            int64_t rtt_us = now_us() - sent_us;
            LOG(INFO) << "[Echo] response: " << response.message() << " | rtt_us=" << rtt_us
                      << " | server=" << response.server_id() << " | latency=" << cntl.latency_us()
                      << "us";
        } else {
            LOG(WARNING) << "[Echo] RPC failed: " << cntl.ErrorText();
        }
    }

    // --- HealthCheck ---
    {
        ::pl::brpc::echo::HealthRequest request;
        ::pl::brpc::echo::HealthResponse response;
        ::brpc::Controller cntl;

        stub.HealthCheck(&cntl, &request, &response, nullptr);

        if (!cntl.Failed()) {
            auto status_name = ::pl::brpc::echo::HealthResponse_Status_Name(response.status());
            LOG(INFO) << "[HealthCheck] status=" << status_name
                      << " | server=" << response.server_id() << " | version=" << response.version()
                      << " | uptime=" << response.uptime_seconds() << "s";
        } else {
            LOG(WARNING) << "[HealthCheck] RPC failed: " << cntl.ErrorText();
        }
    }

    return 0;
}
