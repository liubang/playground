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
#include <butil/logging.h>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <fmt/core.h>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

#include "cpp/pl/minidfs/common/block_token.h"
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
    FileStatus status{
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
    status.file_append_mode =
        proto.file_append_mode() == protocol::FILE_APPEND_MODE_IMMUTABLE_AFTER_COMPLETE
            ? FileAppendMode::kImmutableAfterComplete
            : FileAppendMode::kAppendable;
    if (proto.has_published_identity()) {
        status.published_identity = {
            .inode_id = proto.published_identity().inode_id(),
            .content_generation = proto.published_identity().content_generation(),
            .length = proto.published_identity().length(),
            .checksum = proto.published_identity().checksum(),
            .checksum_valid = proto.published_identity().checksum_valid(),
        };
    }
    return status;
}

inline FileIdentity to_file_identity(const protocol::FileIdentityProto& proto) {
    return FileIdentity{
        .inode_id = proto.inode_id(),
        .content_generation = proto.content_generation(),
        .length = proto.length(),
        .checksum = proto.checksum(),
        .checksum_valid = proto.checksum_valid(),
    };
}

inline bool same_published_identity(const FileIdentity& expected, const FileIdentity& actual) {
    return expected.inode_id == actual.inode_id &&
           expected.content_generation == actual.content_generation &&
           expected.length == actual.length && expected.checksum_valid == actual.checksum_valid &&
           (!expected.checksum_valid || expected.checksum == actual.checksum);
}

inline bool same_published_identity(const protocol::FileIdentityProto& expected,
                                    const FileIdentity& actual) {
    return expected.inode_id() == actual.inode_id &&
           expected.content_generation() == actual.content_generation &&
           expected.length() == actual.length &&
           expected.checksum_valid() == actual.checksum_valid &&
           (!expected.checksum_valid() || expected.checksum() == actual.checksum);
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

std::string DfsClient::next_request_id() {
    const auto seq = request_seq_.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    return fmt::format("{}-{}-{}", config_.client_id, now_ms, seq);
}

bool DfsClient::init() {
    brpc::ChannelOptions options;
    options.timeout_ms = config_.rpc_timeout_ms;
    options.max_retry = config_.max_retry;
    if (namenode_channel_.Init(config_.namenode_address.c_str(), &options) != 0) {
        LOG(ERROR) << "Failed to init channel to NameNode at " << config_.namenode_address;
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
    auto* header = request.mutable_header();
    header->set_request_id(next_request_id());
    header->set_client_id(config_.client_id);

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
    auto* header = request.mutable_header();
    header->set_request_id(next_request_id());
    header->set_client_id(config_.client_id);

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
    auto* header = request.mutable_header();
    header->set_request_id(next_request_id());
    header->set_client_id(config_.client_id);

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

Result<DfsOutputStream> DfsClient::create_output_stream(std::string_view dfs_path,
                                                        bool overwrite,
                                                        FileAppendMode file_append_mode) {
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
    create_req.set_overwrite(overwrite);
    create_req.set_file_append_mode(file_append_mode == FileAppendMode::kImmutableAfterComplete
                                        ? protocol::FILE_APPEND_MODE_IMMUTABLE_AFTER_COMPLETE
                                        : protocol::FILE_APPEND_MODE_APPENDABLE);
    auto* create_header = create_req.mutable_header();
    create_header->set_request_id(next_request_id());
    create_header->set_client_id(config_.client_id);

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

    return DfsOutputStream::create(&namenode_channel_,
                                   create_resp.inode_id(),
                                   config_.client_id,
                                   {
                                       .block_size = config_.block_size,
                                       .chunk_size = config_.chunk_size,
                                       .replication = config_.replication,
                                       .rpc_timeout_ms = config_.rpc_timeout_ms,
                                       .initial_checksum = compute_crc32c(nullptr, 0),
                                       .request_id_prefix = create_header->request_id(),
                                   });
}

Result<DfsOutputStream> DfsClient::create_immutable_output_stream(std::string_view dfs_path,
                                                                  bool overwrite) {
    return create_output_stream(dfs_path, overwrite, FileAppendMode::kImmutableAfterComplete);
}

Result<Void> DfsClient::put(std::string_view local_path,
                            std::string_view dfs_path,
                            bool overwrite) {
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

    auto stream = create_output_stream(dfs_path, overwrite, FileAppendMode::kAppendable);
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
    auto* header = request.mutable_header();
    header->set_request_id(next_request_id());
    header->set_client_id(config_.client_id);
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

    if (!response.has_published_identity()) {
        return pl::makeError(static_cast<status_code_t>(ErrorCode::kInternalError),
                             "AppendFile response missing published identity");
    }
    if (!response.published_identity().checksum_valid()) {
        return pl::makeError(static_cast<status_code_t>(ErrorCode::kChecksumMismatch),
                             "cannot append without a trusted published content checksum");
    }

    auto stream = DfsOutputStream::create(
        &namenode_channel_,
        response.inode_id(),
        config_.client_id,
        {
            .block_size = response.block_size(),
            .chunk_size = config_.chunk_size,
            .replication = response.replication(),
            .rpc_timeout_ms = config_.rpc_timeout_ms,
            .starting_block_index = response.next_block_index(),
            .initial_length = response.published_identity().length(),
            .initial_checksum =
                response.published_identity().checksum_valid()
                    ? std::optional<uint32_t>(response.published_identity().checksum())
                    : std::nullopt,
            .request_id_prefix = header->request_id(),
        });
    if (stream.hasError()) {
        return folly::makeUnexpected(stream.error());
    }
    return copy_to_stream(fd.get(), &stream.value());
}

Result<Void> DfsClient::truncate(std::string_view dfs_path, uint64_t length) {
    protocol::NameNodeService_Stub stub(&namenode_channel_);
    brpc::Controller controller;
    protocol::TruncateFileRequest request;
    request.set_path(std::string(dfs_path));
    request.set_length(length);
    auto* header = request.mutable_header();
    header->set_request_id(next_request_id());
    header->set_client_id(config_.client_id);
    protocol::TruncateFileResponse response;
    stub.TruncateFile(&controller, &request, &response, nullptr);
    if (controller.Failed()) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kRPCError),
                            "TruncateFile RPC failed: {}",
                            controller.ErrorText());
    }
    return check_status(response.status());
}

Result<Void> DfsClient::setrep(std::string_view dfs_path, uint32_t replication) {
    protocol::NameNodeService_Stub stub(&namenode_channel_);
    brpc::Controller controller;
    protocol::SetReplicationRequest request;
    request.set_path(std::string(dfs_path));
    request.set_replication(replication);
    auto* header = request.mutable_header();
    header->set_request_id(next_request_id());
    header->set_client_id(config_.client_id);
    protocol::SetReplicationResponse response;
    stub.SetReplication(&controller, &request, &response, nullptr);
    if (controller.Failed()) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kRPCError),
                            "SetReplication RPC failed: {}",
                            controller.ErrorText());
    }
    return check_status(response.status());
}

// File read

Result<Void> DfsClient::get(std::string_view dfs_path, std::string_view local_path) {
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

    auto data = read_exact(dfs_path, 0, fs.length, fs.published_identity);
    if (data.hasError()) {
        return folly::makeUnexpected(data.error());
    }

    std::ofstream ofs(std::string(local_path), std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kIOError),
                            "Cannot open local file for writing: {}",
                            local_path);
    }
    ofs.write(data.value().data(), static_cast<std::streamsize>(data.value().size()));
    if (!ofs.good()) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kIOError),
                            "Write error on local file: {}",
                            local_path);
    }

    ofs.close();
    RETURN_VOID;
}

Result<std::string> DfsClient::read_exact(std::string_view dfs_path,
                                          uint64_t offset,
                                          uint64_t length) {
    auto stat_result = stat(dfs_path);
    if (stat_result.hasError()) {
        RETURN_ERROR(stat_result);
    }
    const auto& fs = stat_result.value();
    return read_exact_with_status(dfs_path, offset, length, fs, fs.published_identity);
}

Result<std::string> DfsClient::read_exact(std::string_view dfs_path,
                                          uint64_t offset,
                                          uint64_t length,
                                          const FileIdentity& expected_identity) {
    auto stat_result = stat(dfs_path);
    if (stat_result.hasError()) {
        RETURN_ERROR(stat_result);
    }
    const auto& fs = stat_result.value();
    if (expected_identity.inode_id != fs.inode_id) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kInvalidArgument),
                            "expected inode {} does not match path inode {}",
                            expected_identity.inode_id,
                            fs.inode_id);
    }
    return read_exact_with_status(dfs_path, offset, length, fs, expected_identity);
}

Result<std::string> DfsClient::read_exact_with_status(std::string_view dfs_path,
                                                      uint64_t offset,
                                                      uint64_t length,
                                                      const FileStatus& fs,
                                                      const FileIdentity& expected_identity) {
    if (fs.is_dir) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kIsDirectory),
                            "Cannot read a directory: {}",
                            dfs_path);
    }
    if (!same_published_identity(expected_identity, fs.published_identity)) {
        return pl::makeError(static_cast<status_code_t>(ErrorCode::kInvalidArgument),
                             "stale expected file identity");
    }
    if (offset > expected_identity.length || length > expected_identity.length - offset) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kInvalidArgument),
                            "read range [{}, {}) exceeds file length {}",
                            offset,
                            offset + length,
                            expected_identity.length);
    }
    protocol::NameNodeService_Stub stub(&namenode_channel_);
    protocol::GetLocatedBlocksRequest req;
    req.set_inode_id(fs.inode_id);
    auto* expected = req.mutable_expected_file_identity();
    expected->set_inode_id(expected_identity.inode_id);
    expected->set_content_generation(expected_identity.content_generation);
    expected->set_length(expected_identity.length);
    expected->set_checksum(expected_identity.checksum);
    expected->set_checksum_valid(expected_identity.checksum_valid);

    auto validate_identity = [&](const protocol::FileIdentityProto& identity,
                                 std::string_view operation) -> Result<Void> {
        if (!same_published_identity(identity, expected_identity)) {
            return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kInternalError),
                                "{} returned mismatched file identity",
                                operation);
        }
        RETURN_VOID;
    };

    brpc::Controller cntl;
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
    if (!resp.has_file_identity()) {
        return pl::makeError(static_cast<status_code_t>(ErrorCode::kInternalError),
                             "GetLocatedBlocks missing file identity");
    }
    auto initial_identity_ok = validate_identity(resp.file_identity(), "GetLocatedBlocks");
    if (initial_identity_ok.hasError()) {
        return folly::makeUnexpected(initial_identity_ok.error());
    }
    if (length == 0) {
        return std::string{};
    }

    std::string result;
    result.reserve(length);
    uint64_t remaining = length;
    uint64_t current_offset = offset;

    for (int block_pos = 0; block_pos < resp.blocks_size() && remaining > 0; ++block_pos) {
        if (block_token_needs_refresh(resp.blocks(block_pos).block_token(), now_ms(), 1000)) {
            brpc::Controller refresh_cntl;
            protocol::GetLocatedBlocksResponse refreshed;
            stub.GetLocatedBlocks(&refresh_cntl, &req, &refreshed, nullptr);
            if (refresh_cntl.Failed()) {
                return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kRPCError),
                                    "GetLocatedBlocks token refresh failed: {}",
                                    refresh_cntl.ErrorText());
            }
            if (refreshed.status().code() != 0) {
                return pl::makeError(static_cast<status_code_t>(refreshed.status().code()),
                                     refreshed.status().message());
            }
            if (!refreshed.has_file_identity()) {
                return pl::makeError(static_cast<status_code_t>(ErrorCode::kInternalError),
                                     "GetLocatedBlocks token refresh missing file identity");
            }
            auto refreshed_identity_ok =
                validate_identity(refreshed.file_identity(), "GetLocatedBlocks token refresh");
            if (refreshed_identity_ok.hasError()) {
                return folly::makeUnexpected(refreshed_identity_ok.error());
            }
            resp = std::move(refreshed);
            if (block_pos >= resp.blocks_size()) {
                return pl::makeError(static_cast<status_code_t>(ErrorCode::kBlockNotFound),
                                     "file block layout changed during token refresh");
            }
        }

        const auto& block_proto = resp.blocks(block_pos);
        uint64_t block_begin = block_proto.offset();
        uint64_t block_end = block_begin + block_proto.length();
        if (current_offset >= block_end || block_begin >= current_offset + remaining) {
            continue;
        }

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
        if (!block_proto.has_block_token()) {
            return pl::makeError(static_cast<status_code_t>(ErrorCode::kInternalError),
                                 "GetLocatedBlocks returned block without token");
        }
        block.block_token = {
            .block_id = block_proto.block_token().block_id(),
            .generation_stamp = block_proto.block_token().generation_stamp(),
            .inode_id = block_proto.block_token().inode_id(),
            .block_index = block_proto.block_token().block_index(),
            .permissions = block_proto.block_token().permissions(),
            .expires_at_ms = block_proto.block_token().expires_at_ms(),
            .signature = block_proto.block_token().signature(),
        };
        if (!block_proto.block_token().has_file_identity()) {
            return pl::makeError(static_cast<status_code_t>(ErrorCode::kInternalError),
                                 "GetLocatedBlocks returned token without file identity");
        }
        auto token_identity = to_file_identity(block_proto.block_token().file_identity());
        if (!same_published_identity(token_identity, expected_identity)) {
            return pl::makeError(static_cast<status_code_t>(ErrorCode::kInternalError),
                                 "GetLocatedBlocks returned token with mismatched file identity");
        }
        block.block_token.file_identity = std::move(token_identity);

        uint64_t in_block_offset = current_offset > block_begin ? current_offset - block_begin : 0;
        uint64_t can_read =
            std::min<uint64_t>(remaining, block_end - (block_begin + in_block_offset));
        auto chunk = read_block(block, in_block_offset, can_read);
        if (chunk.hasError()) {
            RETURN_ERROR(chunk);
        }
        if (chunk.value().size() != can_read) {
            return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kIOError),
                                "short read: expected {}, got {}",
                                can_read,
                                chunk.value().size());
        }
        result.append(chunk.value());
        current_offset += can_read;
        remaining -= can_read;
    }

    if (remaining != 0) {
        return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kIOError),
                            "short read at file level, {} bytes remaining",
                            remaining);
    }
    if (offset == 0 && length == expected_identity.length && expected_identity.checksum_valid) {
        uint32_t checksum = compute_crc32c(result.data(), result.size());
        if (checksum != expected_identity.checksum) {
            return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kChecksumMismatch),
                                "file checksum mismatch: expected={:#x}, got={:#x}",
                                expected_identity.checksum,
                                checksum);
        }
    }
    return result;
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

Result<std::string> DfsClient::read_block(const LocatedBlock& block,
                                          uint64_t offset,
                                          uint64_t length) {
    if (length == 0) {
        return std::string{};
    }
    // Try each replica location until one succeeds.
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
        req.set_offset(offset);
        req.set_length(length);

        // Attach block token for DataNode authorization
        auto* token = req.mutable_block_token();
        token->set_block_id(block.block_token.block_id);
        token->set_generation_stamp(block.block_token.generation_stamp);
        token->set_inode_id(block.block_token.inode_id);
        token->set_block_index(block.block_token.block_index);
        token->set_permissions(block.block_token.permissions);
        token->set_expires_at_ms(block.block_token.expires_at_ms);
        token->set_signature(block.block_token.signature);
        if (block.block_token.file_identity.has_value()) {
            const auto& identity = block.block_token.file_identity.value();
            auto* token_identity = token->mutable_file_identity();
            token_identity->set_inode_id(identity.inode_id);
            token_identity->set_content_generation(identity.content_generation);
            token_identity->set_length(identity.length);
            token_identity->set_checksum(identity.checksum);
            token_identity->set_checksum_valid(identity.checksum_valid);
        }

        protocol::ReadBlockResponse resp;
        dn_stub.ReadBlock(&cntl, &req, &resp, nullptr);

        if (cntl.Failed()) {
            LOG(WARNING) << "ReadBlock from " << addr << " failed: " << cntl.ErrorText()
                         << ", trying next replica";
            continue;
        }
        if (resp.status().code() != 0) {
            LOG(WARNING) << "ReadBlock from " << addr
                         << " returned error: " << resp.status().message()
                         << ", trying next replica";
            continue;
        }
        if (resp.length() != length || resp.data().size() != length) {
            LOG(WARNING) << "ReadBlock from " << addr << " short read (expected " << length
                         << ", got " << resp.length() << "), trying next replica";
            continue;
        }

        // Verify checksum
        uint32_t computed = compute_crc32c(resp.data().data(), resp.data().size());
        if (computed != resp.checksum()) {
            LOG(WARNING) << "ReadBlock from " << addr << ": checksum mismatch, trying next replica";
            continue;
        }

        return std::string(resp.data());
    }

    return MAKE_ERROR_F(static_cast<status_code_t>(ErrorCode::kNoAvailableDataNode),
                        "No reachable DataNode for block {}",
                        block.block_id);
}

} // namespace pl::minidfs
