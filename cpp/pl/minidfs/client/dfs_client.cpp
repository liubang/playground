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
// Created: 2026/05/10 23:00

#include "cpp/pl/minidfs/client/dfs_client.h"

#include <algorithm>
#include <brpc/channel.h>
#include <brpc/controller.h>
#include <cstdint>
#include <fcntl.h>
#include <fmt/core.h>
#include <folly/logging/xlog.h>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

#include "cpp/pl/minidfs/common/checksum.h"
#include "cpp/pl/minidfs/common/constants.h"
#include "cpp/pl/minidfs/common/error_code.h"
#include "cpp/pl/minidfs/protocol/minidfs.pb.h"

namespace pl::minidfs {

namespace {

/// Convert a StatusProto to a pl::Status error, or return success.
inline Result<Void> check_status(const protocol::StatusProto& sp) {
    if (sp.code() == 0) {
        RETURN_VOID;
    }
    return pl::makeError(static_cast<status_code_t>(sp.code()), sp.message());
}

/// Build a FileStatus from a FileStatusProto.
inline FileStatus to_file_status(const protocol::FileStatusProto& proto) {
    return FileStatus{
        .inode_id = proto.inode_id(),
        .path = proto.path(),
        .is_dir = proto.is_dir(),
        .length = proto.length(),
        .replication = proto.replication(),
        .block_size = proto.block_size(),
        .mtime_ms = proto.mtime_ms(),
        .owner = proto.owner(),
        .group = proto.group(),
        .permission = proto.permission(),
    };
}

// RAII wrapper for file descriptors.
class ScopedFd {
public:
    explicit ScopedFd(int fd) : fd_(fd) {}
    ~ScopedFd() {
        if (fd_ >= 0)
            ::close(fd_);
    }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    int get() const { return fd_; }
    int release() {
        int f = fd_;
        fd_ = -1;
        return f;
    }

private:
    int fd_;
};

Result<Void> copy_to_stream(int fd, DfsOutputStream* stream) {
    std::vector<char> buffer(64 * kKB);
    while (true) {
        auto count = ::read(fd, buffer.data(), buffer.size());
        if (count < 0) {
            return pl::makeError(static_cast<status_code_t>(ErrorCode::kIOError),
                                 "failed to read local file");
        }
        if (count == 0) {
            return stream->close();
        }
        auto written = stream->write(buffer.data(), static_cast<uint64_t>(count));
        if (written.hasError()) {
            return written;
        }
    }
}

} // namespace

// Lifecycle

std::unique_ptr<DfsClient> DfsClient::create(DfsClientConfig config) {
    auto client = std::unique_ptr<DfsClient>(new DfsClient(std::move(config)));
    if (!client->init()) {
        return nullptr;
    }
    return client;
}

DfsClient::DfsClient(DfsClientConfig config) : config_(std::move(config)) {}

DfsClient::~DfsClient() = default;

bool DfsClient::init() {
    brpc::ChannelOptions options;
    options.timeout_ms = config_.rpc_timeout_ms;
    options.max_retry = config_.max_retry;
    if (namenode_channel_.Init(config_.namenode_address.c_str(), &options) != 0) {
        XLOGF(ERR, "Failed to init channel to NameNode at {}", config_.namenode_address);
        return false;
    }
    return true;
}

// Namespace operations

Result<Void> DfsClient::mkdir(std::string_view path) {
    protocol::NameNodeService_Stub stub(&namenode_channel_);
    brpc::Controller cntl;

    protocol::MkdirRequest request;
    request.set_path(std::string(path));
    request.set_owner("dfs");
    request.set_group("dfs");
    request.set_permission(kDefaultPermission);

    protocol::MkdirResponse response;
    stub.Mkdir(&cntl, &request, &response, nullptr);

    if (cntl.Failed()) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kRPCError),
                            "Mkdir RPC failed: {}",
                            cntl.ErrorText());
    }
    return check_status(response.status());
}

Result<FileStatus> DfsClient::stat(std::string_view path) {
    protocol::NameNodeService_Stub stub(&namenode_channel_);
    brpc::Controller cntl;

    protocol::GetFileStatusRequest request;
    request.set_path(std::string(path));

    protocol::GetFileStatusResponse response;
    stub.GetFileStatus(&cntl, &request, &response, nullptr);

    if (cntl.Failed()) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kRPCError),
                            "GetFileStatus RPC failed: {}",
                            cntl.ErrorText());
    }
    if (response.status().code() != 0) {
        return pl::makeError(static_cast<status_code_t>(response.status().code()),
                             response.status().message());
    }
    return to_file_status(response.file_status());
}

Result<std::vector<FileStatus>> DfsClient::ls(std::string_view path) {
    protocol::NameNodeService_Stub stub(&namenode_channel_);
    brpc::Controller cntl;

    protocol::ListStatusRequest request;
    request.set_path(std::string(path));

    protocol::ListStatusResponse response;
    stub.ListStatus(&cntl, &request, &response, nullptr);

    if (cntl.Failed()) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kRPCError),
                            "ListStatus RPC failed: {}",
                            cntl.ErrorText());
    }
    if (response.status().code() != 0) {
        return pl::makeError(static_cast<status_code_t>(response.status().code()),
                             response.status().message());
    }

    std::vector<FileStatus> entries;
    entries.reserve(response.entries_size());
    for (const auto& entry : response.entries()) {
        entries.push_back(to_file_status(entry));
    }
    return entries;
}

Result<Void> DfsClient::rm(std::string_view path, bool recursive) {
    protocol::NameNodeService_Stub stub(&namenode_channel_);
    brpc::Controller cntl;

    protocol::DeleteRequest request;
    request.set_path(std::string(path));
    request.set_recursive(recursive);

    protocol::DeleteResponse response;
    stub.Delete(&cntl, &request, &response, nullptr);

    if (cntl.Failed()) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kRPCError),
                            "Delete RPC failed: {}",
                            cntl.ErrorText());
    }
    return check_status(response.status());
}

Result<Void> DfsClient::mv(std::string_view src, std::string_view dst) {
    protocol::NameNodeService_Stub stub(&namenode_channel_);
    brpc::Controller cntl;

    protocol::RenameRequest request;
    request.set_src(std::string(src));
    request.set_dst(std::string(dst));

    protocol::RenameResponse response;
    stub.Rename(&cntl, &request, &response, nullptr);

    if (cntl.Failed()) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kRPCError),
                            "Rename RPC failed: {}",
                            cntl.ErrorText());
    }
    return check_status(response.status());
}

// File write — pipeline replication

Result<Void> DfsClient::put(std::string_view local_path, std::string_view dfs_path) {
    // 1. Open local file
    ScopedFd fd(::open(std::string(local_path).c_str(), O_RDONLY));
    if (fd.get() < 0) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kIOError),
                            "Cannot open local file: {}",
                            local_path);
    }

    struct stat st{};
    if (::fstat(fd.get(), &st) != 0) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kIOError),
                            "Cannot stat local file: {}",
                            local_path);
    }
    // 2. CreateFile on NameNode
    protocol::NameNodeService_Stub stub(&namenode_channel_);
    brpc::Controller cntl;

    protocol::CreateFileRequest create_req;
    create_req.set_path(std::string(dfs_path));
    create_req.set_owner("dfs");
    create_req.set_group("dfs");
    create_req.set_permission(0644);
    create_req.set_replication(config_.replication);
    create_req.set_block_size(config_.block_size);
    create_req.set_client_id(config_.client_id);

    protocol::CreateFileResponse create_resp;
    stub.CreateFile(&cntl, &create_req, &create_resp, nullptr);

    if (cntl.Failed()) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kRPCError),
                            "CreateFile RPC failed: {}",
                            cntl.ErrorText());
    }
    if (create_resp.status().code() != 0) {
        return pl::makeError(static_cast<status_code_t>(create_resp.status().code()),
                             create_resp.status().message());
    }

    const uint64_t inode_id = create_resp.inode_id();
    auto stream = DfsOutputStream::create(&namenode_channel_,
                                          inode_id,
                                          config_.client_id,
                                          {
                                              .block_size = config_.block_size,
                                              .chunk_size = config_.chunk_size,
                                              .replication = config_.replication,
                                              .rpc_timeout_ms = config_.rpc_timeout_ms,
                                          });
    if (stream.hasError()) {
        return folly::makeUnexpected(stream.error());
    }
    return copy_to_stream(fd.get(), &stream.value());
}

Result<Void> DfsClient::append(std::string_view local_path, std::string_view dfs_path) {
    ScopedFd fd(::open(std::string(local_path).c_str(), O_RDONLY));
    if (fd.get() < 0) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kIOError),
                            "Cannot open local file: {}",
                            local_path);
    }

    protocol::NameNodeService_Stub stub(&namenode_channel_);
    brpc::Controller controller;
    protocol::AppendFileRequest request;
    request.set_path(std::string(dfs_path));
    request.set_client_id(config_.client_id);
    protocol::AppendFileResponse response;
    stub.AppendFile(&controller, &request, &response, nullptr);
    if (controller.Failed()) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kRPCError),
                            "AppendFile RPC failed: {}",
                            controller.ErrorText());
    }
    if (response.status().code() != 0) {
        return pl::makeError(static_cast<status_code_t>(response.status().code()),
                             response.status().message());
    }

    auto stream = DfsOutputStream::create(&namenode_channel_,
                                          response.inode_id(),
                                          config_.client_id,
                                          {
                                              .block_size = response.block_size(),
                                              .chunk_size = config_.chunk_size,
                                              .replication = response.replication(),
                                              .rpc_timeout_ms = config_.rpc_timeout_ms,
                                              .starting_block_index = response.next_block_index(),
                                          });
    if (stream.hasError()) {
        return folly::makeUnexpected(stream.error());
    }
    return copy_to_stream(fd.get(), &stream.value());
}

// File read

Result<Void> DfsClient::get(std::string_view dfs_path, std::string_view local_path) {
    // 1. Get file status to obtain inode_id
    auto stat_result = stat(dfs_path);
    if (stat_result.hasError()) {
        RETURN_ERROR(stat_result);
    }
    const auto& fs = stat_result.value();
    if (fs.is_dir) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kIsDirectory),
                            "Cannot get a directory: {}",
                            dfs_path);
    }

    // 2. GetLocatedBlocks
    protocol::NameNodeService_Stub stub(&namenode_channel_);
    brpc::Controller cntl;

    protocol::GetLocatedBlocksRequest req;
    req.set_inode_id(fs.inode_id);

    protocol::GetLocatedBlocksResponse resp;
    stub.GetLocatedBlocks(&cntl, &req, &resp, nullptr);

    if (cntl.Failed()) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kRPCError),
                            "GetLocatedBlocks RPC failed: {}",
                            cntl.ErrorText());
    }
    if (resp.status().code() != 0) {
        return pl::makeError(static_cast<status_code_t>(resp.status().code()),
                             resp.status().message());
    }

    // 3. Open local file for writing
    std::ofstream ofs(std::string(local_path), std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kIOError),
                            "Cannot open local file for writing: {}",
                            local_path);
    }

    // 4. Read each block
    for (const auto& block_proto : resp.blocks()) {
        LocatedBlock block{
            .block_id = block_proto.block_id(),
            .generation_stamp = block_proto.generation_stamp(),
            .offset = block_proto.offset(),
            .length = block_proto.length(),
        };
        for (const auto& loc : block_proto.locations()) {
            block.locations.push_back(DataNodeEndpoint{
                .datanode_id = loc.datanode_id(),
                .host = loc.host(),
                .data_port = loc.data_port(),
            });
        }

        auto data_result = read_block(block);
        if (data_result.hasError()) {
            RETURN_ERROR(data_result);
        }
        ofs.write(data_result.value().data(),
                  static_cast<std::streamsize>(data_result.value().size()));
        if (!ofs.good()) {
            return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kIOError),
                                "Write error on local file: {}",
                                local_path);
        }
    }

    ofs.close();
    RETURN_VOID;
}

// Admin / diagnostic operations

Result<DfsClient::ClusterInfo> DfsClient::get_cluster_info() {
    protocol::AdminService_Stub stub(&namenode_channel_);
    brpc::Controller cntl;

    protocol::GetClusterInfoRequest request;
    protocol::GetClusterInfoResponse response;
    stub.GetClusterInfo(&cntl, &request, &response, nullptr);

    if (cntl.Failed()) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kRPCError),
                            "GetClusterInfo RPC failed: {}",
                            cntl.ErrorText());
    }
    if (response.status().code() != 0) {
        return pl::makeError(static_cast<status_code_t>(response.status().code()),
                             response.status().message());
    }

    return ClusterInfo{
        .total_capacity_bytes = response.total_capacity_bytes(),
        .used_bytes = response.used_bytes(),
        .free_bytes = response.free_bytes(),
        .live_datanodes = response.live_datanodes(),
        .dead_datanodes = response.dead_datanodes(),
        .total_blocks = response.total_blocks(),
        .total_files = response.total_files(),
        .total_directories = response.total_directories(),
        .under_replicated_blocks = response.under_replicated_blocks(),
    };
}

Result<std::vector<DfsClient::DataNodeSummary>> DfsClient::list_datanodes(bool include_dead) {
    protocol::AdminService_Stub stub(&namenode_channel_);
    brpc::Controller cntl;

    protocol::ListDataNodesRequest request;
    request.set_include_dead(include_dead);

    protocol::ListDataNodesResponse response;
    stub.ListDataNodes(&cntl, &request, &response, nullptr);

    if (cntl.Failed()) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kRPCError),
                            "ListDataNodes RPC failed: {}",
                            cntl.ErrorText());
    }
    if (response.status().code() != 0) {
        return pl::makeError(static_cast<status_code_t>(response.status().code()),
                             response.status().message());
    }

    std::vector<DataNodeSummary> result;
    result.reserve(response.datanodes_size());
    for (const auto& dn : response.datanodes()) {
        result.push_back(DataNodeSummary{
            .datanode_id = dn.datanode_id(),
            .uuid = dn.uuid(),
            .hostname = dn.hostname(),
            .ip = dn.ip(),
            .rpc_port = dn.rpc_port(),
            .data_port = dn.data_port(),
            .rack = dn.rack(),
            .state = dn.state(),
            .capacity_bytes = dn.capacity_bytes(),
            .used_bytes = dn.used_bytes(),
            .free_bytes = dn.free_bytes(),
            .last_heartbeat_ms = dn.last_heartbeat_ms(),
            .block_count = dn.block_count(),
        });
    }
    return result;
}

Result<DfsClient::DataNodeSummary> DfsClient::get_datanode_info(uint64_t datanode_id) {
    protocol::AdminService_Stub stub(&namenode_channel_);
    brpc::Controller cntl;

    protocol::GetDataNodeInfoRequest request;
    request.set_datanode_id(datanode_id);

    protocol::GetDataNodeInfoResponse response;
    stub.GetDataNodeInfo(&cntl, &request, &response, nullptr);

    if (cntl.Failed()) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kRPCError),
                            "GetDataNodeInfo RPC failed: {}",
                            cntl.ErrorText());
    }
    if (response.status().code() != 0) {
        return pl::makeError(static_cast<status_code_t>(response.status().code()),
                             response.status().message());
    }

    const auto& dn = response.datanode();
    return DataNodeSummary{
        .datanode_id = dn.datanode_id(),
        .uuid = dn.uuid(),
        .hostname = dn.hostname(),
        .ip = dn.ip(),
        .rpc_port = dn.rpc_port(),
        .data_port = dn.data_port(),
        .rack = dn.rack(),
        .state = dn.state(),
        .capacity_bytes = dn.capacity_bytes(),
        .used_bytes = dn.used_bytes(),
        .free_bytes = dn.free_bytes(),
        .last_heartbeat_ms = dn.last_heartbeat_ms(),
        .block_count = dn.block_count(),
    };
}

Result<DfsClient::DataNodeSummary> DfsClient::get_datanode_info_by_uuid(std::string_view uuid) {
    protocol::AdminService_Stub stub(&namenode_channel_);
    brpc::Controller cntl;

    protocol::GetDataNodeInfoRequest request;
    request.set_uuid(std::string(uuid));

    protocol::GetDataNodeInfoResponse response;
    stub.GetDataNodeInfo(&cntl, &request, &response, nullptr);

    if (cntl.Failed()) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kRPCError),
                            "GetDataNodeInfo RPC failed: {}",
                            cntl.ErrorText());
    }
    if (response.status().code() != 0) {
        return pl::makeError(static_cast<status_code_t>(response.status().code()),
                             response.status().message());
    }

    const auto& dn = response.datanode();
    return DataNodeSummary{
        .datanode_id = dn.datanode_id(),
        .uuid = dn.uuid(),
        .hostname = dn.hostname(),
        .ip = dn.ip(),
        .rpc_port = dn.rpc_port(),
        .data_port = dn.data_port(),
        .rack = dn.rack(),
        .state = dn.state(),
        .capacity_bytes = dn.capacity_bytes(),
        .used_bytes = dn.used_bytes(),
        .free_bytes = dn.free_bytes(),
        .last_heartbeat_ms = dn.last_heartbeat_ms(),
        .block_count = dn.block_count(),
    };
}

namespace {

DfsClient::InodeDetail to_inode_detail(const protocol::InodeInfoProto& proto) {
    return DfsClient::InodeDetail{
        .inode_id = proto.inode_id(),
        .type = proto.type(),
        .parent_id = proto.parent_id(),
        .name = proto.name(),
        .owner = proto.owner(),
        .group = proto.group(),
        .permission = proto.permission(),
        .length = proto.length(),
        .replication = proto.replication(),
        .block_size = proto.block_size(),
        .state = proto.state(),
        .ctime_ms = proto.ctime_ms(),
        .mtime_ms = proto.mtime_ms(),
        .block_count = proto.block_count(),
        .child_count = proto.child_count(),
    };
}

} // namespace

Result<DfsClient::InodeDetail> DfsClient::get_inode_info(uint64_t inode_id) {
    protocol::AdminService_Stub stub(&namenode_channel_);
    brpc::Controller cntl;

    protocol::GetInodeInfoRequest request;
    request.set_inode_id(inode_id);

    protocol::GetInodeInfoResponse response;
    stub.GetInodeInfo(&cntl, &request, &response, nullptr);

    if (cntl.Failed()) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kRPCError),
                            "GetInodeInfo RPC failed: {}",
                            cntl.ErrorText());
    }
    if (response.status().code() != 0) {
        return pl::makeError(static_cast<status_code_t>(response.status().code()),
                             response.status().message());
    }
    return to_inode_detail(response.inode());
}

Result<DfsClient::InodeDetail> DfsClient::get_inode_info_by_path(std::string_view path) {
    protocol::AdminService_Stub stub(&namenode_channel_);
    brpc::Controller cntl;

    protocol::GetInodeInfoRequest request;
    request.set_path(std::string(path));

    protocol::GetInodeInfoResponse response;
    stub.GetInodeInfo(&cntl, &request, &response, nullptr);

    if (cntl.Failed()) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kRPCError),
                            "GetInodeInfo RPC failed: {}",
                            cntl.ErrorText());
    }
    if (response.status().code() != 0) {
        return pl::makeError(static_cast<status_code_t>(response.status().code()),
                             response.status().message());
    }
    return to_inode_detail(response.inode());
}

namespace {

std::vector<DfsClient::FileBlockDetail> to_file_blocks(
    const google::protobuf::RepeatedPtrField<protocol::FileBlockInfoProto>& blocks_proto) {
    std::vector<DfsClient::FileBlockDetail> blocks;
    blocks.reserve(blocks_proto.size());
    for (const auto& b : blocks_proto) {
        DfsClient::FileBlockDetail detail{
            .block_id = b.block_id(),
            .block_index = b.block_index(),
            .generation_stamp = b.generation_stamp(),
            .length = b.length(),
            .state = b.state(),
            .desired_replicas = b.desired_replicas(),
            .actual_replicas = b.actual_replicas(),
        };
        for (const auto& loc : b.locations()) {
            detail.locations.push_back(DfsClient::BlockLocation{
                .datanode_id = loc.datanode_id(),
                .host = loc.host(),
                .data_port = loc.data_port(),
            });
        }
        blocks.push_back(std::move(detail));
    }
    return blocks;
}

} // namespace

Result<std::vector<DfsClient::FileBlockDetail>> DfsClient::get_file_blocks(uint64_t inode_id) {
    protocol::AdminService_Stub stub(&namenode_channel_);
    brpc::Controller cntl;

    protocol::GetFileBlocksRequest request;
    request.set_inode_id(inode_id);

    protocol::GetFileBlocksResponse response;
    stub.GetFileBlocks(&cntl, &request, &response, nullptr);

    if (cntl.Failed()) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kRPCError),
                            "GetFileBlocks RPC failed: {}",
                            cntl.ErrorText());
    }
    if (response.status().code() != 0) {
        return pl::makeError(static_cast<status_code_t>(response.status().code()),
                             response.status().message());
    }
    return to_file_blocks(response.blocks());
}

Result<std::vector<DfsClient::FileBlockDetail>> DfsClient::get_file_blocks_by_path(
    std::string_view path) {
    protocol::AdminService_Stub stub(&namenode_channel_);
    brpc::Controller cntl;

    protocol::GetFileBlocksRequest request;
    request.set_path(std::string(path));

    protocol::GetFileBlocksResponse response;
    stub.GetFileBlocks(&cntl, &request, &response, nullptr);

    if (cntl.Failed()) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kRPCError),
                            "GetFileBlocks RPC failed: {}",
                            cntl.ErrorText());
    }
    if (response.status().code() != 0) {
        return pl::makeError(static_cast<status_code_t>(response.status().code()),
                             response.status().message());
    }
    return to_file_blocks(response.blocks());
}

Result<DfsClient::BlockDetail> DfsClient::get_block_info(uint64_t block_id) {
    protocol::AdminService_Stub stub(&namenode_channel_);
    brpc::Controller cntl;

    protocol::GetBlockInfoRequest request;
    request.set_block_id(block_id);

    protocol::GetBlockInfoResponse response;
    stub.GetBlockInfo(&cntl, &request, &response, nullptr);

    if (cntl.Failed()) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kRPCError),
                            "GetBlockInfo RPC failed: {}",
                            cntl.ErrorText());
    }
    if (response.status().code() != 0) {
        return pl::makeError(static_cast<status_code_t>(response.status().code()),
                             response.status().message());
    }

    BlockDetail detail{
        .block_id = response.block_id(),
        .inode_id = response.inode_id(),
        .block_index = response.block_index(),
        .generation_stamp = response.generation_stamp(),
        .length = response.length(),
        .state = response.state(),
        .desired_replicas = response.desired_replicas(),
    };
    for (const auto& r : response.replicas()) {
        detail.replicas.push_back(ReplicaDetail{
            .datanode_id = r.datanode_id(),
            .hostname = r.hostname(),
            .state = r.state(),
            .length = r.length(),
            .generation_stamp = r.generation_stamp(),
            .report_time_ms = r.report_time_ms(),
        });
    }
    return detail;
}

// File read

Result<std::string> DfsClient::read_block(const LocatedBlock& block) {
    // Try each replica location until one succeeds
    for (const auto& loc : block.locations) {
        brpc::Channel dn_channel;
        brpc::ChannelOptions options;
        options.timeout_ms = config_.rpc_timeout_ms * 2;
        options.max_retry = 1;

        std::string addr = loc.host + ":" + std::to_string(loc.data_port);
        if (dn_channel.Init(addr.c_str(), &options) != 0) {
            continue; // try next replica
        }

        protocol::DataTransferService_Stub dn_stub(&dn_channel);
        brpc::Controller cntl;

        protocol::ReadBlockRequest req;
        req.set_block_id(block.block_id);
        req.set_generation_stamp(block.generation_stamp);
        req.set_offset(0);
        req.set_length(0); // 0 = read entire block

        protocol::ReadBlockResponse resp;
        dn_stub.ReadBlock(&cntl, &req, &resp, nullptr);

        if (cntl.Failed()) {
            XLOGF(
                WARN, "ReadBlock from {} failed: {}, trying next replica", addr, cntl.ErrorText());
            continue;
        }
        if (resp.status().code() != 0) {
            XLOGF(WARN,
                  "ReadBlock from {} returned error: {}, trying next replica",
                  addr,
                  resp.status().message());
            continue;
        }

        // Verify checksum
        uint32_t computed = compute_crc32c(resp.data().data(), resp.data().size());
        if (computed != resp.checksum()) {
            XLOGF(WARN, "ReadBlock from {}: checksum mismatch, trying next replica", addr);
            continue;
        }

        return std::string(resp.data());
    }

    return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kNoAvailableDataNode),
                        "No reachable DataNode for block {}",
                        block.block_id);
}

} // namespace pl::minidfs
