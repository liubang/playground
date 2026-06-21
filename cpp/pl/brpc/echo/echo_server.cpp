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

#include <brpc/server.h>
#include <chrono>
#include <gflags/gflags.h>
#include <memory>
#include <string>

#include "cpp/pl/brpc/echo/proto/echo.pb.h"

DEFINE_int32(port, 8000, "TCP Port of this server");
DEFINE_string(server_id, "cpp-brpc-server", "Unique server identifier");
DEFINE_int32(idle_timeout_s,
             -1,
             "Connection will be closed if there is no "
             "read/write operations during the last "
             "`idle_timeout_s`");

namespace pl::brpc_echo {

class EchoServiceImpl : public ::pl::brpc::echo::EchoService {
public:
    EchoServiceImpl() : start_time_(std::chrono::steady_clock::now()) {}
    ~EchoServiceImpl() override = default;

    void Echo(::google::protobuf::RpcController* cntl_base,
              const ::pl::brpc::echo::EchoRequest* request,
              ::pl::brpc::echo::EchoResponse* response,
              ::google::protobuf::Closure* done) override {
        ::brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<::brpc::Controller*>(cntl_base);

        LOG(INFO) << "Echo request[log_id=" << cntl->log_id() << "] from " << cntl->remote_side()
                  << " to " << cntl->local_side() << ": " << request->message();

        auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();

        response->set_message(request->message());
        response->set_original_timestamp(request->timestamp_us());
        response->set_server_timestamp(now_us);
        response->set_server_id(FLAGS_server_id);
    }

    void HealthCheck(::google::protobuf::RpcController* cntl_base,
                     const ::pl::brpc::echo::HealthRequest* /*request*/,
                     ::pl::brpc::echo::HealthResponse* response,
                     ::google::protobuf::Closure* done) override {
        ::brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<::brpc::Controller*>(cntl_base);

        auto uptime_sec = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - start_time_)
                              .count();

        LOG(INFO) << "HealthCheck request[log_id=" << cntl->log_id() << "] from "
                  << cntl->remote_side();

        response->set_status(::pl::brpc::echo::HealthResponse::SERVING);
        response->set_server_id(FLAGS_server_id);
        response->set_version("1.0.0");
        response->set_uptime_seconds(uptime_sec);
    }

private:
    std::chrono::steady_clock::time_point start_time_;
};

} // namespace pl::brpc_echo

int main(int argc, char* argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    ::brpc::Server server;

    pl::brpc_echo::EchoServiceImpl echo_service_impl;

    if (server.AddService(&echo_service_impl, ::brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        LOG(ERROR) << "Fail to add service";
        return -1;
    }

    ::brpc::ServerOptions options;
    options.idle_timeout_sec = FLAGS_idle_timeout_s;

    if (server.Start(FLAGS_port, &options) != 0) {
        LOG(ERROR) << "Fail to start EchoServer";
        return -1;
    }

    LOG(INFO) << "[C++ brpc EchoServer] Listening on port " << FLAGS_port
              << " (server_id: " << FLAGS_server_id << ")";

    server.RunUntilAskedToQuit();
    return 0;
}
