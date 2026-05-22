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

#include <brpc/server.h>
#include <gflags/gflags.h>

#include "cpp/pl/brpc/proto/echo.pb.h"

DEFINE_int32(port, 8000, "TCP Port of this server");
DEFINE_int32(idle_timeout_s,
             -1,
             "Connection will be closed if there is no "
             "read/write operations during the last "
             "`idle_timeout_s`");

namespace pl::brpc_echo {

// 实现 EchoService 接口
class EchoServiceImpl : public ::pl::brpc::proto::EchoService {
public:
    EchoServiceImpl() = default;
    ~EchoServiceImpl() override = default;

    void Echo(::google::protobuf::RpcController* cntl_base,
              const ::pl::brpc::proto::EchoRequest* request,
              ::pl::brpc::proto::EchoResponse* response,
              ::google::protobuf::Closure* done) override {
        // RAII 方式确保 done->Run() 在函数退出时被调用
        ::brpc::ClosureGuard done_guard(done);

        ::brpc::Controller* cntl = static_cast<::brpc::Controller*>(cntl_base);

        LOG(INFO) << "Received request[log_id=" << cntl->log_id() << "] from "
                  << cntl->remote_side() << " to " << cntl->local_side() << ": "
                  << request->message();

        // 将请求消息原样回显
        response->set_message(request->message());
    }
};

} // namespace pl::brpc_echo

int main(int argc, char* argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    ::brpc::Server server;

    pl::brpc_echo::EchoServiceImpl echo_service_impl;

    // 将服务添加到 server，第二个参数 SERVER_DOESNT_OWN_SERVICE 表示 server
    // 不负责 service 的生命周期管理
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

    // 等待直到 Ctrl-C 被按下，然后 Stop() 和 Join() server
    server.RunUntilAskedToQuit();
    return 0;
}
