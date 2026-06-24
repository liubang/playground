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
// Created: 2026/06/23 23:49

#include <braft/node.h>
#include <braft/node_manager.h>
#include <braft/raft.h>
#include <brpc/server.h>
#include <butil/endpoint.h>
#include <butil/logging.h>
#include <gflags/gflags.h>
#include <string>

#include "cpp/pl/minitable/master/admin_service_impl.h"
#include "cpp/pl/minitable/master/master_service_impl.h"
#include "cpp/pl/minitable/master/master_sm.h"
#include "cpp/pl/minitable/master/unit_service_impl.h"

DEFINE_string(group_id, "minitable_master", "braft group id");
DEFINE_string(
    conf,
    "",
    "braft initial configuration, e.g. '127.0.0.1:8100:0,127.0.0.1:8101:0,127.0.0.1:8102:0'");
DEFINE_string(peer_id, "", "this instance's peer id, e.g. '127.0.0.1:8100:0'");
DEFINE_int32(port, 8100, "brpc listen port");

int main(int argc, char* argv[]) {
    google::ParseCommandLineFlags(&argc, &argv, true);

    if (FLAGS_conf.empty() || FLAGS_peer_id.empty()) {
        LOG(ERROR) << "conf and peer_id are required";
        return -1;
    }

    // 1. 创建 MasterSM (braft::StateMachine)
    auto sm = std::make_unique<pl::minitable::master::MasterSM>();

    // 2. 初始化 braft Node
    if (sm->init(FLAGS_group_id, FLAGS_peer_id, FLAGS_conf) != 0) {
        LOG(ERROR) << "Failed to init MasterSM";
        return -1;
    }

    // 3. 创建三个独立的 service 实现
    namespace mm = pl::minitable::master;
    mm::MasterServiceImpl master_svc(sm.get());
    mm::AdminServiceImpl  admin_svc(sm.get());
    mm::UnitServiceImpl   unit_svc(sm.get());

    // Phase 1: 共用 brpc Server, 后续可各自绑定独立端口
    brpc::Server server;
    if (server.AddService(&master_svc, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        LOG(ERROR) << "Failed to add MasterService";
        return -1;
    }
    if (server.AddService(&admin_svc, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        LOG(ERROR) << "Failed to add AdminService";
        return -1;
    }
    if (server.AddService(&unit_svc, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        LOG(ERROR) << "Failed to add UnitService";
        return -1;
    }

    // 添加 braft 内置服务 (raft_cli 管理命令)
    if (::braft::add_service(&server, FLAGS_port) != 0) {
        LOG(ERROR) << "Failed to add braft service";
        return -1;
    }

    brpc::ServerOptions options;
    if (server.Start(FLAGS_port, &options) != 0) {
        LOG(ERROR) << "Failed to start brpc server on port " << FLAGS_port;
        return -1;
    }

    // 4. 启动 braft Node (在 brpc server 启动之后)
    if (sm->start() != 0) {
        LOG(ERROR) << "Failed to start MasterSM braft node";
        server.Stop(0);
        server.Join();
        return -1;
    }

    LOG(INFO) << "minitable Master started on port " << FLAGS_port << ", group=" << FLAGS_group_id
              << ", peer=" << FLAGS_peer_id;

    // 5. 等待退出信号
    server.RunUntilAskedToQuit();

    // 6. 优雅关闭
    LOG(INFO) << "Master shutting down...";
    sm->shutdown();
    server.Stop(0);
    server.Join();

    return 0;
}
