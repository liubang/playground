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
// Created: 2026/07/15 18:53

#include <algorithm>
#include <array>
#include <barrier>
#include <brpc/channel.h>
#include <brpc/controller.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <gflags/gflags.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <unordered_set>
#include <vector>

#include "cpp/pl/minidfs/client/dfs_client.h"
#include "cpp/pl/minidfs/common/block_token.h"
#include "cpp/pl/minidfs/common/checksum.h"
#include "cpp/pl/minidfs/common/error_code.h"
#include "cpp/pl/minidfs/protocol/minidfs.pb.h"

DEFINE_string(namenode, "127.0.0.1:19000", "NameNode address");
DEFINE_string(work_dir, "/tmp", "Parent directory for local E2E files");
DEFINE_string(dfs_root, "/api-e2e", "DFS root used by this test");
DEFINE_uint64(block_size, 1048576, "Block size used by the E2E client");
DEFINE_uint32(replication, 3, "Replication factor used by the E2E client");

namespace pl::minidfs::e2e {
namespace {

class Failure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw Failure(std::string(message));
    }
}

template <typename T> T require_value(Result<T> result, std::string_view operation) {
    if (result.hasError()) {
        throw Failure(std::string(operation) + " failed: " + result.error().describe());
    }
    return std::move(result).value();
}

void require_ok(Result<Void> result, std::string_view operation) {
    if (result.hasError()) {
        throw Failure(std::string(operation) + " failed: " + result.error().describe());
    }
}

void require_error(Result<Void> result, std::string_view operation) {
    if (!result.hasError()) {
        throw Failure(std::string(operation) + " unexpectedly succeeded");
    }
}

template <typename T> void require_error(Result<T> result, std::string_view operation) {
    if (!result.hasError()) {
        throw Failure(std::string(operation) + " unexpectedly succeeded");
    }
}

class TempDirectory final {
public:
    explicit TempDirectory(const std::filesystem::path& parent) {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = parent /
                ("minidfs-api-e2e-" + std::to_string(::getpid()) + "-" + std::to_string(nonce));
        std::filesystem::create_directories(path_);
    }

    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

void write_pattern_file(const std::filesystem::path& path, uint64_t size) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(output.is_open(), "cannot create local test file");
    std::vector<char> buffer(64 * 1024);
    uint64_t offset = 0;
    while (offset < size) {
        const auto count = static_cast<size_t>(std::min<uint64_t>(buffer.size(), size - offset));
        for (size_t index = 0; index < count; ++index) {
            buffer[index] = static_cast<char>((offset + index) * 31U + 17U);
        }
        output.write(buffer.data(), static_cast<std::streamsize>(count));
        require(output.good(), "cannot write local test file");
        offset += count;
    }
}

void write_text_file(const std::filesystem::path& path, std::string_view content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(output.is_open(), "cannot create local text file");
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    require(output.good(), "cannot write local text file");
}

std::vector<char> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.is_open(), "cannot open local result file");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void require_same_file(const std::filesystem::path& expected,
                       const std::filesystem::path& actual,
                       std::string_view operation) {
    require(read_file(expected) == read_file(actual),
            std::string(operation) + " produced different bytes");
}

bool contains_path(const std::vector<FileStatus>& entries, std::string_view path) {
    return std::ranges::any_of(entries,
                               [path](const FileStatus& entry) { return entry.path == path; });
}

std::string unique_id(std::string_view prefix) {
    static uint64_t sequence = 0;
    return std::string(prefix) + "-" + std::to_string(::getpid()) + "-" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
           std::to_string(++sequence);
}

void initialize_channel(brpc::Channel* channel, std::string_view address) {
    brpc::ChannelOptions options;
    options.timeout_ms = 10000;
    options.max_retry = 0;
    require(channel->Init(std::string(address).c_str(), &options) == 0,
            "failed to initialize brpc channel for " + std::string(address));
}

void require_rpc_ok(const brpc::Controller& controller, std::string_view operation) {
    require(!controller.Failed(),
            std::string(operation) + " RPC failed: " + controller.ErrorText());
}

void set_header(protocol::RequestHeader* header,
                std::string_view client_id,
                std::string_view request_id) {
    header->set_client_id(std::string(client_id));
    header->set_request_id(std::string(request_id));
}

class DfsClientE2E final {
public:
    DfsClientE2E() : temp_dir_(FLAGS_work_dir), root_(FLAGS_dfs_root) {
        require(!root_.empty() && root_.front() == '/' && root_ != "/",
                "dfs_root must be an absolute, non-root path");
        require(FLAGS_block_size > 0, "block_size must be greater than zero");
        require(FLAGS_replication > 0, "replication must be greater than zero");
        DfsClientConfig config;
        config.namenode_address = FLAGS_namenode;
        config.rpc_timeout_ms = 10000;
        config.max_retry = 3;
        config.client_id = "api-e2e-" + std::to_string(::getpid());
        config.replication = FLAGS_replication;
        config.block_size = FLAGS_block_size;
        client_id_ = config.client_id;
        client_ = DfsClient::create(std::move(config));
        require(client_ != nullptr, "DfsClient::create returned nullptr");
        initialize_channel(&namenode_channel_, FLAGS_namenode);
    }

    void run() {
        cleanup();
        try {
            verify_cluster_and_datanodes();
            verify_namespace();
            verify_file_sizes_and_diagnostics();
            verify_read_block_tokens();
            verify_allocate_block_semantics();
            verify_rename_safety();
            verify_mutations_and_errors();
            cleanup();
        } catch (...) {
            cleanup();
            throw;
        }
    }

private:
    void cleanup() {
        auto status = client_->stat(root_);
        if (!status.hasError()) {
            require_ok(client_->rm(root_, true), "cleanup DFS root");
        }
    }

    void verify_cluster_and_datanodes() {
        std::cout << "[API E2E] cluster and DataNode APIs\n";
        auto cluster = require_value(client_->get_cluster_info(), "get_cluster_info");
        require(cluster.live_datanodes == 3, "expected exactly three live DataNodes");

        auto datanodes = require_value(client_->list_datanodes(false), "list_datanodes(false)");
        require(datanodes.size() == 3, "expected three active DataNode summaries");
        auto all_datanodes = require_value(client_->list_datanodes(true), "list_datanodes(true)");
        require(all_datanodes.size() >= datanodes.size(), "all DataNodes omitted a live node");

        for (const auto& datanode : datanodes) {
            require(datanode.state == "live", "DataNode is not live");
            auto by_id = require_value(client_->get_datanode_info(datanode.datanode_id),
                                       "get_datanode_info");
            auto by_uuid = require_value(client_->get_datanode_info_by_uuid(datanode.uuid),
                                         "get_datanode_info_by_uuid");
            require(by_id.uuid == datanode.uuid, "DataNode ID lookup returned the wrong UUID");
            require(by_uuid.datanode_id == datanode.datanode_id,
                    "DataNode UUID lookup returned the wrong ID");
        }
    }

    void verify_namespace() {
        std::cout << "[API E2E] namespace APIs\n";
        const auto deepest = root_ + "/level1/level2/level3";
        const auto data_dir = root_ + "/level1/level2/data";
        require_ok(client_->mkdir(deepest), "mkdir nested path");

        auto status = require_value(client_->stat(deepest), "stat directory");
        require(status.is_dir, "nested path is not a directory");
        auto entries = require_value(client_->ls(root_ + "/level1/level2"), "ls parent");
        require(contains_path(entries, deepest), "ls omitted nested directory");

        require_ok(client_->mv(deepest, data_dir), "mv directory");
        require_error(client_->stat(deepest), "stat old directory path");
        status = require_value(client_->stat(data_dir), "stat moved directory");
        require(status.is_dir, "moved path is not a directory");

        auto inode_by_path =
            require_value(client_->get_inode_info_by_path(data_dir), "get_inode_info_by_path(dir)");
        auto inode_by_id =
            require_value(client_->get_inode_info(inode_by_path.inode_id), "get_inode_info(dir)");
        require(inode_by_id.type == "directory", "directory inode has the wrong type");
    }

    void verify_file_sizes_and_diagnostics() {
        std::cout << "[API E2E] file-size, transfer, inode, and block APIs\n";
        const std::vector<std::pair<std::string, uint64_t>> cases = {
            {"empty", 0},
            {"one-byte", 1},
            {"block-minus-one", FLAGS_block_size - 1},
            {"exact-block", FLAGS_block_size},
            {"block-plus-one", FLAGS_block_size + 1},
            {"multi-block", 2 * FLAGS_block_size + 33},
        };
        const auto data_dir = root_ + "/level1/level2/data";

        for (const auto& [name, size] : cases) {
            const auto source = temp_dir_.path() / (name + ".source");
            const auto downloaded = temp_dir_.path() / (name + ".download");
            const auto dfs_path = data_dir + "/" + name + ".bin";
            write_pattern_file(source, size);
            require_ok(client_->put(source.string(), dfs_path), "put " + name);
            require_ok(client_->get(dfs_path, downloaded.string()), "get " + name);
            require_same_file(source, downloaded, "put/get " + name);

            const auto status = require_value(client_->stat(dfs_path), "stat " + name);
            require(status.length == size, "stat returned an incorrect file length");
            require(status.replication == FLAGS_replication,
                    "stat returned an incorrect replication factor");
        }

        const auto target = data_dir + "/multi-block.bin";
        auto inode_by_path =
            require_value(client_->get_inode_info_by_path(target), "get_inode_info_by_path(file)");
        auto inode_by_id =
            require_value(client_->get_inode_info(inode_by_path.inode_id), "get_inode_info(file)");
        require(inode_by_id.name == "multi-block.bin", "file inode lookup returned wrong name");
        require(inode_by_id.block_count == 3, "multi-block file did not produce three blocks");

        auto blocks_by_path =
            require_value(client_->get_file_blocks_by_path(target), "get_file_blocks_by_path");
        auto blocks_by_id =
            require_value(client_->get_file_blocks(inode_by_path.inode_id), "get_file_blocks");
        require(blocks_by_path.size() == 3, "path block lookup returned the wrong count");
        require(blocks_by_id.size() == blocks_by_path.size(),
                "inode block lookup returned a different count");
        for (const auto& block : blocks_by_path) {
            require(block.desired_replicas == FLAGS_replication,
                    "block desired replication is incorrect");
            require(block.actual_replicas == FLAGS_replication,
                    "block actual replication is incorrect");
            require(block.locations.size() == FLAGS_replication, "block locations are incomplete");
            auto detail = require_value(client_->get_block_info(block.block_id), "get_block_info");
            require(detail.inode_id == inode_by_path.inode_id,
                    "block detail returned the wrong inode");
            require(detail.replicas.size() == FLAGS_replication,
                    "block detail replicas are incomplete");
        }
    }

    void verify_read_block_tokens() {
        std::cout << "[API E2E] direct ReadBlock token authorization\n";
        const auto path = root_ + "/level1/level2/data/exact-block.bin";
        const auto inode =
            require_value(client_->get_inode_info_by_path(path), "lookup token file");

        protocol::NameNodeService_Stub namenode(&namenode_channel_);
        protocol::GetLocatedBlocksRequest located_request;
        located_request.set_inode_id(inode.inode_id);
        protocol::GetLocatedBlocksResponse located_response;
        brpc::Controller located_controller;
        namenode.GetLocatedBlocks(
            &located_controller, &located_request, &located_response, nullptr);
        require_rpc_ok(located_controller, "GetLocatedBlocks for token test");
        require(located_response.status().code() == 0,
                "GetLocatedBlocks failed: " + located_response.status().message());
        require(located_response.blocks_size() == 1, "token test file must have one block");

        const auto& block = located_response.blocks(0);
        require(block.locations_size() > 0, "token test block has no live location");
        require(
            has_token_permission(block.block_token().permissions(), BlockTokenPermission::kRead),
            "NameNode did not issue a read token");
        const auto& location = block.locations(0);
        brpc::Channel datanode_channel;
        initialize_channel(&datanode_channel,
                           location.host() + ":" + std::to_string(location.data_port()));
        protocol::DataTransferService_Stub datanode(&datanode_channel);

        auto read = [&](const protocol::BlockTokenProto& token) {
            protocol::ReadBlockRequest request;
            request.set_block_id(block.block_id());
            request.set_generation_stamp(block.generation_stamp());
            request.set_offset(0);
            request.set_length(0);
            *request.mutable_block_token() = token;
            protocol::ReadBlockResponse response;
            brpc::Controller controller;
            datanode.ReadBlock(&controller, &request, &response, nullptr);
            require_rpc_ok(controller, "direct ReadBlock");
            return response;
        };

        auto valid = read(block.block_token());
        require(valid.status().code() == 0, "valid ReadBlock token was rejected");
        require(valid.length() == FLAGS_block_size, "direct ReadBlock returned wrong length");

        auto tampered_signature = block.block_token();
        require(!tampered_signature.signature().empty(), "read token signature is empty");
        auto signature = tampered_signature.signature();
        signature.front() = static_cast<char>(signature.front() ^ 0x01);
        tampered_signature.set_signature(signature);

        auto no_permissions = block.block_token();
        no_permissions.set_permissions(0);
        auto expired = block.block_token();
        expired.set_expires_at_ms(1);
        for (const auto& [name, response] :
             std::array<std::pair<std::string_view, protocol::ReadBlockResponse>, 3>{
                 {{"tampered signature", read(tampered_signature)},
                  {"permissions=0", read(no_permissions)},
                  {"expires_at_ms=1", read(expired)}}}) {
            require(response.status().code() ==
                        static_cast<uint32_t>(ErrorCode::kInvalidBlockToken),
                    std::string(name) + " did not return kInvalidBlockToken");
        }
    }

    protocol::CreateFileResponse create_raw_file(std::string_view path,
                                                 std::string_view client_id,
                                                 std::string_view request_id) {
        protocol::CreateFileRequest request;
        request.set_path(std::string(path));
        request.set_owner("api-e2e");
        request.set_group("api-e2e");
        request.set_permission(0644);
        request.set_replication(FLAGS_replication);
        request.set_block_size(FLAGS_block_size);
        request.set_client_id(std::string(client_id));
        set_header(request.mutable_header(), client_id, request_id);
        protocol::CreateFileResponse response;
        brpc::Controller controller;
        protocol::NameNodeService_Stub stub(&namenode_channel_);
        stub.CreateFile(&controller, &request, &response, nullptr);
        require_rpc_ok(controller, "CreateFile");
        return response;
    }

    static protocol::AllocateBlockResponse allocate_raw(brpc::Channel* channel,
                                                        uint64_t inode_id,
                                                        std::string_view client_id,
                                                        std::string_view request_id) {
        protocol::AllocateBlockRequest request;
        request.set_inode_id(inode_id);
        request.set_block_index(0);
        request.set_replication(FLAGS_replication);
        request.set_client_id(std::string(client_id));
        set_header(request.mutable_header(), client_id, request_id);
        protocol::AllocateBlockResponse response;
        brpc::Controller controller;
        protocol::NameNodeService_Stub stub(channel);
        stub.AllocateBlock(&controller, &request, &response, nullptr);
        require_rpc_ok(controller, "AllocateBlock");
        return response;
    }

    void verify_write_only_token(const protocol::LocatedBlockProto& block,
                                 uint64_t inode_id,
                                 std::string_view operation) {
        const auto& token = block.block_token();
        require(token.block_id() == block.block_id() &&
                    token.generation_stamp() == block.generation_stamp() &&
                    token.inode_id() == inode_id && token.block_index() == 0,
                std::string(operation) + " returned a token bound to the wrong block");
        require(token.expires_at_ms() > 1 && !token.signature().empty(),
                std::string(operation) + " returned an invalid token");
        require(token.permissions() == kBlockTokenPermissionWrite,
                std::string(operation) + " token is not write-only");
    }

    void commit_raw_block(const protocol::LocatedBlockProto& block, uint64_t inode_id) {
        require(block.locations_size() > 0, "allocated block has no DataNode locations");
        const std::string payload = "e2e";
        protocol::WriteBlockRequest write_request;
        write_request.set_block_id(block.block_id());
        write_request.set_inode_id(inode_id);
        write_request.set_block_index(0);
        write_request.set_generation_stamp(block.generation_stamp());
        for (int index = 1; index < block.locations_size(); ++index) {
            *write_request.add_pipeline() = block.locations(index);
        }
        write_request.set_data(payload);
        write_request.set_chunk_index(0);
        write_request.set_checksum(compute_crc32c(payload.data(), payload.size()));
        write_request.set_is_last_chunk(true);
        *write_request.mutable_block_token() = block.block_token();

        const auto& primary = block.locations(0);
        brpc::Channel datanode_channel;
        initialize_channel(&datanode_channel,
                           primary.host() + ":" + std::to_string(primary.data_port()));
        protocol::DataTransferService_Stub datanode(&datanode_channel);
        protocol::WriteBlockResponse write_response;
        brpc::Controller write_controller;
        datanode.WriteBlock(&write_controller, &write_request, &write_response, nullptr);
        require_rpc_ok(write_controller, "WriteBlock for lease release");
        require(write_response.status().code() == 0,
                "WriteBlock for lease release failed: " + write_response.status().message());

        protocol::CommitBlockRequest commit_request;
        commit_request.set_block_id(block.block_id());
        commit_request.set_length(payload.size());
        commit_request.set_generation_stamp(block.generation_stamp());
        for (const auto& location : block.locations()) {
            commit_request.add_finalized_datanode_ids(location.datanode_id());
        }
        protocol::CommitBlockResponse commit_response;
        brpc::Controller commit_controller;
        protocol::DataNodeProtocolService_Stub namenode(&namenode_channel_);
        namenode.CommitBlock(&commit_controller, &commit_request, &commit_response, nullptr);
        require_rpc_ok(commit_controller, "CommitBlock for lease release");
        require(commit_response.status().code() == 0,
                "CommitBlock for lease release failed: " + commit_response.status().message());
    }

    void complete_raw_file(uint64_t inode_id, std::string_view client_id) {
        protocol::CompleteFileRequest request;
        request.set_inode_id(inode_id);
        request.set_client_id(std::string(client_id));
        set_header(request.mutable_header(), client_id, unique_id("complete-file"));
        protocol::CompleteFileResponse response;
        brpc::Controller controller;
        protocol::NameNodeService_Stub stub(&namenode_channel_);
        stub.CompleteFile(&controller, &request, &response, nullptr);
        require_rpc_ok(controller, "CompleteFile");
        require(response.status().code() == 0,
                "CompleteFile failed: " + response.status().message());
    }

    void verify_allocate_block_semantics() {
        std::cout << "[API E2E] direct AllocateBlock idempotency and concurrency\n";
        const auto data_dir = root_ + "/level1/level2/data";

        const auto replay_client = unique_id("allocate-replay-client");
        const auto replay_request = unique_id("allocate-replay-request");
        const auto replay_path = data_dir + "/allocate-replay.bin";
        auto created = create_raw_file(replay_path, replay_client, unique_id("create-replay"));
        require(created.status().code() == 0, "raw replay file creation failed");
        auto first =
            allocate_raw(&namenode_channel_, created.inode_id(), replay_client, replay_request);
        require(first.status().code() == 0 && first.has_block(), "first allocation failed");
        verify_write_only_token(first.block(), created.inode_id(), "first allocation");
        auto replay =
            allocate_raw(&namenode_channel_, created.inode_id(), replay_client, replay_request);
        require(replay.status().code() == 0 && replay.has_block(), "allocation replay failed");
        require(replay.block().block_id() == first.block().block_id(),
                "allocation replay returned a different block_id");
        verify_write_only_token(replay.block(), created.inode_id(), "allocation replay");

        const auto race_client = unique_id("allocate-race-client");
        const auto race_path = data_dir + "/allocate-race.bin";
        auto race_file = create_raw_file(race_path, race_client, unique_id("create-race"));
        require(race_file.status().code() == 0, "raw race file creation failed");
        std::barrier start(3);
        auto contender = [&](std::string request_id) {
            brpc::Channel channel;
            initialize_channel(&channel, FLAGS_namenode);
            start.arrive_and_wait();
            return allocate_raw(&channel, race_file.inode_id(), race_client, request_id);
        };
        auto left = std::async(std::launch::async, contender, unique_id("allocate-left"));
        auto right = std::async(std::launch::async, contender, unique_id("allocate-right"));
        start.arrive_and_wait();
        auto left_response = left.get();
        auto right_response = right.get();
        const auto success_count = static_cast<int>(left_response.status().code() == 0) +
                                   static_cast<int>(right_response.status().code() == 0);
        require(success_count == 1, "concurrent allocations did not produce exactly one winner");
        const auto& winner = left_response.status().code() == 0 ? left_response : right_response;
        verify_write_only_token(winner.block(), race_file.inode_id(), "concurrent allocation");

        commit_raw_block(first.block(), created.inode_id());
        complete_raw_file(created.inode_id(), replay_client);
        auto replay_after_complete =
            allocate_raw(&namenode_channel_, created.inode_id(), replay_client, replay_request);
        require(replay_after_complete.status().code() != 0,
                "successful AllocateBlock request replay succeeded after lease release");
    }

    protocol::RenameResponse rename_raw(std::string_view src,
                                        std::string_view dst,
                                        std::string_view request_id) {
        protocol::RenameRequest request;
        request.set_src(std::string(src));
        request.set_dst(std::string(dst));
        set_header(request.mutable_header(), client_id_, request_id);
        protocol::RenameResponse response;
        brpc::Controller controller;
        protocol::NameNodeService_Stub stub(&namenode_channel_);
        stub.Rename(&controller, &request, &response, nullptr);
        require_rpc_ok(controller, "Rename");
        return response;
    }

    void require_acyclic_tree(std::string_view root) {
        std::vector<std::string> pending = {std::string(root)};
        std::unordered_set<uint64_t> visited;
        size_t traversed = 0;
        while (!pending.empty()) {
            auto path = std::move(pending.back());
            pending.pop_back();
            const auto inode =
                require_value(client_->get_inode_info_by_path(path), "traverse " + path);
            require(visited.insert(inode.inode_id).second,
                    "namespace traversal visited an inode twice (cycle detected)");
            require(++traversed <= 3, "namespace traversal exceeded expected directory count");
            for (const auto& child : require_value(client_->ls(path), "list " + path)) {
                require(child.is_dir, "rename safety tree unexpectedly contains a file");
                pending.push_back(child.path);
            }
        }
        require(traversed == 3, "rename safety traversal lost a directory");
    }

    void verify_rename_safety() {
        std::cout << "[API E2E] rename cycle prevention and concurrent cross rename\n";
        const auto base = root_ + "/rename-safety";
        require_ok(client_->mkdir(base + "/self/child/grandchild"), "create self-move tree");
        require(
            rename_raw(base + "/self", base + "/self", unique_id("rename-self")).status().code() !=
                0,
            "moving a directory to itself succeeded");
        require(rename_raw(base + "/self",
                           base + "/self/child/grandchild/self",
                           unique_id("rename-descendant"))
                        .status()
                        .code() != 0,
                "moving a directory below its descendant succeeded");

        const auto race = base + "/race";
        require_ok(client_->mkdir(race + "/a"), "create rename race a");
        require_ok(client_->mkdir(race + "/b"), "create rename race b");
        std::barrier start(3);
        auto contender = [&](std::string src, std::string dst, std::string request_id) {
            brpc::Channel channel;
            initialize_channel(&channel, FLAGS_namenode);
            protocol::RenameRequest request;
            request.set_src(std::move(src));
            request.set_dst(std::move(dst));
            set_header(request.mutable_header(), client_id_, request_id);
            start.arrive_and_wait();
            protocol::RenameResponse response;
            brpc::Controller controller;
            protocol::NameNodeService_Stub stub(&channel);
            stub.Rename(&controller, &request, &response, nullptr);
            require_rpc_ok(controller, "concurrent Rename");
            return response;
        };
        auto left = std::async(
            std::launch::async, contender, race + "/a", race + "/b/a", unique_id("rename-left"));
        auto right = std::async(
            std::launch::async, contender, race + "/b", race + "/a/b", unique_id("rename-right"));
        start.arrive_and_wait();
        const auto left_response = left.get();
        const auto right_response = right.get();
        const auto success_count = static_cast<int>(left_response.status().code() == 0) +
                                   static_cast<int>(right_response.status().code() == 0);
        require(success_count == 1, "cross rename did not produce exactly one winner");
        require_acyclic_tree(race);
    }

    void verify_mutations_and_errors() {
        std::cout << "[API E2E] append, truncate, overwrite, setrep, mv, rm, and errors\n";
        const auto data_dir = root_ + "/level1/level2/data";
        const auto target = data_dir + "/multi-block.bin";
        const auto source = temp_dir_.path() / "multi-block.source";
        const auto append = temp_dir_.path() / "append.txt";
        const auto appended = temp_dir_.path() / "appended.download";
        const auto expected_appended = temp_dir_.path() / "appended.expected";
        write_text_file(append, "api append payload\n");

        require_ok(client_->append(append.string(), target), "append");
        auto expected = read_file(source);
        const auto suffix = read_file(append);
        expected.insert(expected.end(), suffix.begin(), suffix.end());
        std::ofstream expected_output(expected_appended, std::ios::binary | std::ios::trunc);
        expected_output.write(expected.data(), static_cast<std::streamsize>(expected.size()));
        expected_output.close();
        require_ok(client_->get(target, appended.string()), "get appended file");
        require_same_file(expected_appended, appended, "append");

        require_ok(client_->truncate(target, FLAGS_block_size), "truncate");
        const auto truncated = temp_dir_.path() / "truncated.download";
        require_ok(client_->get(target, truncated.string()), "get truncated file");
        auto truncated_bytes = read_file(truncated);
        auto source_bytes = read_file(source);
        require(truncated_bytes.size() == FLAGS_block_size, "truncate returned the wrong size");
        require(std::equal(truncated_bytes.begin(), truncated_bytes.end(), source_bytes.begin()),
                "truncate returned the wrong prefix");

        const auto replacement = temp_dir_.path() / "replacement.txt";
        const auto replacement_download = temp_dir_.path() / "replacement.download";
        write_text_file(replacement, "replacement through DfsClient API\n");
        require_error(client_->put(replacement.string(), target, false),
                      "put existing file without overwrite");
        require_ok(client_->put(replacement.string(), target, true), "put overwrite");
        require_ok(client_->setrep(target, 2), "setrep");
        auto status = require_value(client_->stat(target), "stat after setrep");
        require(status.replication == 2, "setrep did not update the inode");
        require_ok(client_->get(target, replacement_download.string()), "get overwritten file");
        require_same_file(replacement, replacement_download, "overwrite");

        const auto moved = data_dir + "/moved.bin";
        require_ok(client_->mv(data_dir + "/one-byte.bin", moved), "mv file");
        require_ok(client_->rm(moved, false), "rm file");
        require_error(client_->stat(moved), "stat removed file");
        require_error(client_->rm(root_ + "/level1", false), "non-recursive rm directory");
        require_error(client_->get(data_dir, (temp_dir_.path() / "directory").string()),
                      "get directory");
        require_error(client_->stat(root_ + "/missing"), "stat missing path");
    }

private:
    TempDirectory temp_dir_;
    std::string root_;
    std::string client_id_;
    std::unique_ptr<DfsClient> client_;
    brpc::Channel namenode_channel_;
};

} // namespace
} // namespace pl::minidfs::e2e

int main(int argc, char** argv) {
    gflags::SetUsageMessage("MiniDFS DfsClient API end-to-end test");
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    try {
        pl::minidfs::e2e::DfsClientE2E test;
        test.run();
        std::cout << "[PASS] MiniDFS DfsClient API E2E passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] MiniDFS DfsClient API E2E: " << error.what() << '\n';
        return 1;
    }
}
