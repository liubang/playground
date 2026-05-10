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
// Created: 2026/05/10 18:30

#include "cpp/pl/minidfs/common/error_code.h"
#include "cpp/pl/minidfs/datanode/block_reporter.h"
#include "cpp/pl/minidfs/datanode/data_transfer_service_impl.h"
#include "cpp/pl/minidfs/datanode/heartbeat_sender.h"
#include "cpp/pl/minidfs/datanode/local_block_store.h"
#include "cpp/pl/minidfs/datanode/replication_worker.h"
#include "cpp/pl/minidfs/protocol/minidfs.pb.h"
#include <brpc/channel.h>
#include <brpc/server.h>
#include <fmt/format.h>
#include <folly/logging/xlog.h>
#include <gflags/gflags.h>

DEFINE_int32(port, 9100, "DataNode data transfer service port");
DEFINE_string(storage_root, "/tmp/minidfs/dn", "Block storage root directory");
DEFINE_string(namenode_addr, "127.0.0.1:9000", "NameNode RPC address");
DEFINE_string(uuid, "", "DataNode UUID (auto-generated if empty)");
DEFINE_string(hostname, "localhost", "DataNode hostname");
DEFINE_string(ip, "127.0.0.1", "DataNode IP address");
DEFINE_int32(rpc_port, 9100, "DataNode RPC port (same as --port)");
DEFINE_string(rack, "/default-rack", "DataNode rack location");
DEFINE_int32(heartbeat_interval_ms, 3000, "Heartbeat interval in milliseconds");
DEFINE_int32(block_report_interval_ms, 600000, "Block report interval in milliseconds");
DEFINE_int32(replication_threads, 4, "Number of replication worker threads");

namespace {

/// Generate a simple UUID if not provided.
std::string generate_uuid() {
    static std::atomic<uint64_t> counter{0};
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return fmt::format("dn-{}-{}", ms, counter.fetch_add(1));
}

} // namespace

int main(int argc, char* argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    std::string uuid = FLAGS_uuid.empty() ? generate_uuid() : FLAGS_uuid;

    // Initialize local block store
    pl::minidfs::LocalBlockStore::Config store_config{
        .storage_root = FLAGS_storage_root,
    };
    pl::minidfs::LocalBlockStore store(store_config);
    auto init_result = store.init();
    if (init_result.hasError()) {
        XLOGF(FATAL, "failed to initialize block store: {}", init_result.error().describe());
        return 1;
    }

    // Connect to NameNode
    brpc::Channel nn_channel;
    brpc::ChannelOptions channel_opts;
    channel_opts.timeout_ms = 5000;
    if (nn_channel.Init(FLAGS_namenode_addr.c_str(), &channel_opts) != 0) {
        XLOGF(FATAL, "failed to connect to NameNode at {}", FLAGS_namenode_addr);
        return 1;
    }

    pl::minidfs::protocol::DataNodeProtocolService_Stub nn_stub(&nn_channel);

    // Register with NameNode
    pl::minidfs::protocol::RegisterDataNodeRequest reg_req;
    reg_req.set_uuid(uuid);
    reg_req.set_hostname(FLAGS_hostname);
    reg_req.set_ip(FLAGS_ip);
    reg_req.set_rpc_port(static_cast<uint32_t>(FLAGS_rpc_port));
    reg_req.set_data_port(static_cast<uint32_t>(FLAGS_port));
    reg_req.set_rack(FLAGS_rack);

    auto avail = store.available_bytes();
    reg_req.set_capacity_bytes(avail.hasValue() ? avail.value() : 0);

    pl::minidfs::protocol::RegisterDataNodeResponse reg_resp;
    brpc::Controller reg_cntl;
    nn_stub.RegisterDataNode(&reg_cntl, &reg_req, &reg_resp, nullptr);
    if (reg_cntl.Failed()) {
        XLOGF(FATAL, "RegisterDataNode RPC failed: {}", reg_cntl.ErrorText());
        return 1;
    }
    if (reg_resp.status().code() != 0) {
        XLOGF(FATAL, "RegisterDataNode failed: {}", reg_resp.status().message());
        return 1;
    }

    uint64_t datanode_id = reg_resp.datanode_id();
    XLOGF(INFO, "registered with NameNode, datanode_id={}", datanode_id);

    // Setup heartbeat sender
    pl::minidfs::HeartbeatFunc heartbeat_func =
        [&nn_stub](uint64_t dn_id, uint64_t capacity, uint64_t used,
                   uint64_t free) -> pl::Result<std::vector<pl::minidfs::HeartbeatCommand>> {
        pl::minidfs::protocol::HeartbeatRequest req;
        req.set_datanode_id(dn_id);
        req.set_capacity_bytes(capacity);
        req.set_used_bytes(used);
        req.set_free_bytes(free);

        pl::minidfs::protocol::HeartbeatResponse resp;
        brpc::Controller cntl;
        cntl.set_timeout_ms(3000);
        nn_stub.Heartbeat(&cntl, &req, &resp, nullptr);
        if (cntl.Failed()) {
            return pl::makeError(static_cast<pl::status_code_t>(pl::minidfs::ErrorCode::kRPCError),
                                 std::string(cntl.ErrorText()));
        }

        std::vector<pl::minidfs::HeartbeatCommand> commands;
        for (const auto& cmd_proto : resp.commands()) {
            pl::minidfs::HeartbeatCommand cmd;
            cmd.type = static_cast<pl::minidfs::CommandType>(cmd_proto.type());
            cmd.block_id = cmd_proto.block_id();
            cmd.generation_stamp = cmd_proto.generation_stamp();
            cmd.target_host = cmd_proto.target_host();
            cmd.target_port = cmd_proto.target_port();
            commands.push_back(std::move(cmd));
        }
        return commands;
    };

    // Setup replication worker
    pl::minidfs::CopyFunc copy_func = [](uint64_t block_id, uint64_t generation_stamp,
                                         const std::string& data, const std::string& target_host,
                                         uint32_t target_port) -> pl::Result<pl::Void> {
        // Connect to target DN
        brpc::Channel target_channel;
        brpc::ChannelOptions opts;
        opts.timeout_ms = 30000;
        std::string addr = fmt::format("{}:{}", target_host, target_port);
        if (target_channel.Init(addr.c_str(), &opts) != 0) {
            return pl::makeError(
                static_cast<pl::status_code_t>(pl::minidfs::ErrorCode::kRPCConnectFailed),
                fmt::format("cannot connect to {}", addr));
        }

        pl::minidfs::protocol::DataTransferService_Stub stub(&target_channel);
        pl::minidfs::protocol::TransferBlockRequest req;
        req.set_block_id(block_id);
        req.set_generation_stamp(generation_stamp);
        req.set_data(data);

        pl::minidfs::protocol::TransferBlockResponse resp;
        brpc::Controller cntl;
        stub.TransferBlock(&cntl, &req, &resp, nullptr);
        if (cntl.Failed()) {
            return pl::makeError(static_cast<pl::status_code_t>(pl::minidfs::ErrorCode::kRPCError),
                                 std::string(cntl.ErrorText()));
        }
        if (resp.status().code() != 0) {
            return pl::makeError(static_cast<pl::status_code_t>(resp.status().code()),
                                 resp.status().message());
        }
        RETURN_VOID;
    };

    pl::minidfs::ReplicationWorker::Config rw_config{
        .max_concurrent_tasks = static_cast<uint32_t>(FLAGS_replication_threads),
    };
    pl::minidfs::ReplicationWorker replication_worker(rw_config, &store, copy_func);

    // Command handler dispatches to replication worker
    pl::minidfs::CommandHandler cmd_handler =
        [&replication_worker](const pl::minidfs::HeartbeatCommand& cmd) {
            pl::minidfs::DataNodeTask task;
            task.block_id = cmd.block_id;
            task.generation_stamp = cmd.generation_stamp;
            task.target_host = cmd.target_host;
            task.target_port = cmd.target_port;
            switch (cmd.type) {
                case pl::minidfs::CommandType::kReplicate:
                    task.kind = pl::minidfs::TaskKind::kCopy;
                    replication_worker.enqueue(std::move(task));
                    break;
                case pl::minidfs::CommandType::kDelete:
                case pl::minidfs::CommandType::kInvalidate:
                    task.kind = pl::minidfs::TaskKind::kDelete;
                    replication_worker.enqueue(std::move(task));
                    break;
                default:
                    break;
            }
        };

    pl::minidfs::HeartbeatSender::Config hb_config{
        .datanode_id = datanode_id,
        .interval_ms = static_cast<uint64_t>(FLAGS_heartbeat_interval_ms),
    };
    pl::minidfs::HeartbeatSender heartbeat_sender(hb_config, &store, heartbeat_func, cmd_handler);

    // Setup block reporter
    pl::minidfs::BlockReportFunc report_func =
        [&nn_stub](uint64_t dn_id, const std::vector<pl::minidfs::BlockInfo>& blocks)
        -> pl::Result<pl::minidfs::BlockReportResponse> {
        pl::minidfs::protocol::BlockReportRequest req;
        req.set_datanode_id(dn_id);
        for (const auto& b : blocks) {
            auto* bp = req.add_blocks();
            bp->set_block_id(b.block_id);
            bp->set_generation_stamp(b.generation_stamp);
            bp->set_length(b.length);
        }

        pl::minidfs::protocol::BlockReportResponse resp;
        brpc::Controller cntl;
        cntl.set_timeout_ms(10000);
        nn_stub.BlockReport(&cntl, &req, &resp, nullptr);
        if (cntl.Failed()) {
            return pl::makeError(static_cast<pl::status_code_t>(pl::minidfs::ErrorCode::kRPCError),
                                 std::string(cntl.ErrorText()));
        }

        pl::minidfs::BlockReportResponse result;
        for (uint64_t bid : resp.blocks_to_delete()) {
            result.blocks_to_delete.push_back(bid);
        }
        return result;
    };

    pl::minidfs::DeleteBlockFunc delete_func = [&store](uint64_t block_id,
                                                        uint64_t generation_stamp) {
        store.delete_block(block_id, generation_stamp);
    };

    pl::minidfs::BlockReporter::Config br_config{
        .datanode_id = datanode_id,
        .full_report_interval_ms = static_cast<uint64_t>(FLAGS_block_report_interval_ms),
    };
    pl::minidfs::BlockReporter block_reporter(br_config, &store, report_func, delete_func);

    // Create data transfer service
    pl::minidfs::DataTransferServiceImpl data_transfer_service(&store, &block_reporter);

    // Start brpc server
    brpc::Server server;
    if (server.AddService(&data_transfer_service, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        XLOG(FATAL, "failed to add DataTransferService");
        return 1;
    }

    brpc::ServerOptions options;
    if (server.Start(FLAGS_port, &options) != 0) {
        XLOGF(FATAL, "failed to start DataNode server on port {}", FLAGS_port);
        return 1;
    }

    // Start background workers
    heartbeat_sender.start();
    block_reporter.start();
    replication_worker.start();

    XLOGF(INFO, "MiniDFS DataNode started on port {}, datanode_id={}, storage={}", FLAGS_port,
          datanode_id, FLAGS_storage_root);

    server.RunUntilAskedToQuit();

    // Graceful shutdown
    replication_worker.stop();
    block_reporter.stop();
    heartbeat_sender.stop();

    return 0;
}
