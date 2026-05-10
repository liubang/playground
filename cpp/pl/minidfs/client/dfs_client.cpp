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

#include "cpp/pl/minidfs/common/constants.h"
#include "cpp/pl/minidfs/common/error_code.h"
#include "cpp/pl/minidfs/protocol/minidfs.pb.h"
#include <algorithm>
#include <brpc/channel.h>
#include <brpc/controller.h>
#include <crc32c/crc32c.h>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fmt/core.h>
#include <folly/logging/xlog.h>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

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

} // namespace

// =============================================================================
// Lifecycle
// =============================================================================

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

// =============================================================================
// Namespace operations
// =============================================================================

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
                            "Mkdir RPC failed: {}", cntl.ErrorText());
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
                            "GetFileStatus RPC failed: {}", cntl.ErrorText());
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
                            "ListStatus RPC failed: {}", cntl.ErrorText());
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
                            "Delete RPC failed: {}", cntl.ErrorText());
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
                            "Rename RPC failed: {}", cntl.ErrorText());
    }
    return check_status(response.status());
}

// =============================================================================
// File write — pipeline replication
// =============================================================================

Result<Void> DfsClient::put(std::string_view local_path, std::string_view dfs_path) {
    // 1. Open local file
    int fd = ::open(std::string(local_path).c_str(), O_RDONLY);
    if (fd < 0) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kIOError),
                            "Cannot open local file: {}", local_path);
    }

    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kIOError),
                            "Cannot stat local file: {}", local_path);
    }
    const uint64_t file_size = static_cast<uint64_t>(st.st_size);

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
        ::close(fd);
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kRPCError),
                            "CreateFile RPC failed: {}", cntl.ErrorText());
    }
    if (create_resp.status().code() != 0) {
        ::close(fd);
        return pl::makeError(static_cast<status_code_t>(create_resp.status().code()),
                             create_resp.status().message());
    }

    const uint64_t inode_id = create_resp.inode_id();

    // 3. Write blocks
    uint64_t remaining = file_size;
    uint32_t block_index = 0;
    std::vector<char> buf(config_.block_size);

    while (remaining > 0) {
        const uint64_t to_read = std::min(remaining, config_.block_size);
        uint64_t bytes_read = 0;

        while (bytes_read < to_read) {
            auto n = ::read(fd, buf.data() + bytes_read, to_read - bytes_read);
            if (n <= 0) {
                ::close(fd);
                return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kIOError),
                                    "Read error on local file at offset {}",
                                    file_size - remaining + bytes_read);
            }
            bytes_read += static_cast<uint64_t>(n);
        }

        auto result = write_block(inode_id, block_index, buf.data(), bytes_read);
        if (result.hasError()) {
            ::close(fd);
            RETURN_ERROR(result);
        }

        remaining -= bytes_read;
        ++block_index;
    }
    ::close(fd);

    // 4. CompleteFile
    cntl.Reset();
    protocol::CompleteFileRequest complete_req;
    complete_req.set_inode_id(inode_id);
    complete_req.set_client_id(config_.client_id);

    protocol::CompleteFileResponse complete_resp;
    stub.CompleteFile(&cntl, &complete_req, &complete_resp, nullptr);

    if (cntl.Failed()) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kRPCError),
                            "CompleteFile RPC failed: {}", cntl.ErrorText());
    }
    return check_status(complete_resp.status());
}

Result<uint64_t> DfsClient::write_block(uint64_t inode_id,
                                        uint32_t block_index,
                                        const char* data,
                                        uint64_t length) {
    // Allocate block from NameNode
    protocol::NameNodeService_Stub nn_stub(&namenode_channel_);
    brpc::Controller cntl;

    protocol::AllocateBlockRequest alloc_req;
    alloc_req.set_inode_id(inode_id);
    alloc_req.set_block_index(block_index);
    alloc_req.set_replication(config_.replication);

    protocol::AllocateBlockResponse alloc_resp;
    nn_stub.AllocateBlock(&cntl, &alloc_req, &alloc_resp, nullptr);

    if (cntl.Failed()) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kRPCError),
                            "AllocateBlock RPC failed: {}", cntl.ErrorText());
    }
    if (alloc_resp.status().code() != 0) {
        return pl::makeError(static_cast<status_code_t>(alloc_resp.status().code()),
                             alloc_resp.status().message());
    }

    const auto& located_block = alloc_resp.block();
    if (located_block.locations_size() == 0) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kNoAvailableDataNode),
                            "No DataNode locations for block {}", located_block.block_id());
    }

    // Connect to first DataNode in the pipeline
    const auto& primary = located_block.locations(0);
    brpc::Channel dn_channel;
    brpc::ChannelOptions dn_options;
    dn_options.timeout_ms = config_.rpc_timeout_ms * 2; // writes may be slower
    dn_options.max_retry = 1;

    std::string dn_addr = primary.host() + ":" + std::to_string(primary.data_port());
    if (dn_channel.Init(dn_addr.c_str(), &dn_options) != 0) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kRPCConnectFailed),
                            "Cannot connect to DataNode {}", dn_addr);
    }

    protocol::DataTransferService_Stub dn_stub(&dn_channel);

    // Send data in chunks
    const uint32_t chunk_size = config_.chunk_size;
    uint64_t offset = 0;
    uint32_t chunk_index = 0;

    while (offset < length) {
        const uint32_t this_chunk =
            static_cast<uint32_t>(std::min<uint64_t>(chunk_size, length - offset));

        brpc::Controller write_cntl;
        protocol::WriteBlockRequest write_req;
        write_req.set_block_id(located_block.block_id());
        write_req.set_inode_id(inode_id);
        write_req.set_block_index(block_index);
        write_req.set_generation_stamp(located_block.generation_stamp());

        // Build pipeline: remaining nodes after the primary
        for (int i = 1; i < located_block.locations_size(); ++i) {
            auto* target = write_req.add_pipeline();
            target->set_datanode_id(located_block.locations(i).datanode_id());
            target->set_host(located_block.locations(i).host());
            target->set_data_port(located_block.locations(i).data_port());
        }

        write_req.set_data(data + offset, this_chunk);
        write_req.set_chunk_index(chunk_index);
        write_req.set_checksum(crc32c::Crc32c(data + offset, this_chunk));
        write_req.set_is_last_chunk(offset + this_chunk >= length);

        protocol::WriteBlockResponse write_resp;
        dn_stub.WriteBlock(&write_cntl, &write_req, &write_resp, nullptr);

        if (write_cntl.Failed()) {
            return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kRPCError),
                                "WriteBlock RPC failed at chunk {}: {}", chunk_index,
                                write_cntl.ErrorText());
        }
        if (write_resp.status().code() != 0) {
            return pl::makeError(static_cast<status_code_t>(write_resp.status().code()),
                                 write_resp.status().message());
        }

        offset += this_chunk;
        ++chunk_index;
    }

    return length;
}

// =============================================================================
// File read
// =============================================================================

Result<Void> DfsClient::get(std::string_view dfs_path, std::string_view local_path) {
    // 1. Get file status to obtain inode_id
    auto stat_result = stat(dfs_path);
    if (stat_result.hasError()) {
        RETURN_ERROR(stat_result);
    }
    const auto& fs = stat_result.value();
    if (fs.is_dir) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kIsDirectory),
                            "Cannot get a directory: {}", dfs_path);
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
                            "GetLocatedBlocks RPC failed: {}", cntl.ErrorText());
    }
    if (resp.status().code() != 0) {
        return pl::makeError(static_cast<status_code_t>(resp.status().code()),
                             resp.status().message());
    }

    // 3. Open local file for writing
    std::ofstream ofs(std::string(local_path), std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kIOError),
                            "Cannot open local file for writing: {}", local_path);
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
                                "Write error on local file: {}", local_path);
        }
    }

    ofs.close();
    RETURN_VOID;
}

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
            XLOGF(WARN, "ReadBlock from {} failed: {}, trying next replica", addr,
                  cntl.ErrorText());
            continue;
        }
        if (resp.status().code() != 0) {
            XLOGF(WARN, "ReadBlock from {} returned error: {}, trying next replica", addr,
                  resp.status().message());
            continue;
        }

        // Verify checksum
        uint32_t computed = crc32c::Crc32c(resp.data().data(), resp.data().size());
        if (computed != resp.checksum()) {
            XLOGF(WARN, "ReadBlock from {}: checksum mismatch, trying next replica", addr);
            continue;
        }

        return std::string(resp.data());
    }

    return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kNoAvailableDataNode),
                        "No reachable DataNode for block {}", block.block_id);
}

} // namespace pl::minidfs
