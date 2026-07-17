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

#pragma once

#include <atomic>
#include <brpc/channel.h>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/pl/minidfs/client/dfs_output_stream.h"
#include "cpp/pl/minidfs/common/types.h"
#include "cpp/pl/status/result.h"

namespace pl::minidfs {

/// Configuration for DfsClient.
struct DfsClientConfig {
    std::string namenode_address; // host:port
    int32_t rpc_timeout_ms = 5000;
    int32_t max_retry = 3;
    std::string client_id; // unique per client instance

    // Write parameters
    uint32_t replication = kDefaultReplication;
    uint64_t block_size = kDefaultBlockSize;
    uint32_t chunk_size = kDefaultChunkSize;
};

/// DfsClient provides a high-level API for interacting with MiniDFS.
/// It encapsulates all NameNode and DataNode RPC calls behind a simple interface.
class DfsClient {
public:
    /// Factory method — returns nullptr on failure.
    [[nodiscard]] static std::unique_ptr<DfsClient> create(DfsClientConfig config);

    ~DfsClient();

    DfsClient(const DfsClient&) = delete;
    DfsClient& operator=(const DfsClient&) = delete;

    // Namespace operations

    /// Create a directory (and parents) at the given path.
    [[nodiscard]] Result<Void> mkdir(std::string_view path);

    /// Get file/directory status.
    [[nodiscard]] Result<FileStatus> stat(std::string_view path);

    /// List directory entries.
    [[nodiscard]] Result<std::vector<FileStatus>> ls(std::string_view path);

    /// Delete a file or directory.
    [[nodiscard]] Result<Void> rm(std::string_view path, bool recursive = false);

    /// Rename/move a file or directory.
    [[nodiscard]] Result<Void> mv(std::string_view src, std::string_view dst);

    // File read/write operations

    /// Create an immutable-after-complete output stream.
    /// Existing closed files may be replaced when overwrite=true.
    [[nodiscard]] Result<DfsOutputStream> create_immutable_output_stream(
        std::string_view dfs_path,
        bool overwrite = false);

    /// Write a local file to DFS using pipeline replication.
    /// Existing closed files may be replaced when overwrite=true.
    [[nodiscard]] Result<Void> put(std::string_view local_path,
                                   std::string_view dfs_path,
                                   bool overwrite = false);

    /// Append a local file to an existing DFS file.
    [[nodiscard]] Result<Void> append(std::string_view local_path, std::string_view dfs_path);

    /// Shrink an existing DFS file.
    [[nodiscard]] Result<Void> truncate(std::string_view dfs_path, uint64_t length);

    /// Change an existing DFS file's desired replication factor.
    [[nodiscard]] Result<Void> setrep(std::string_view dfs_path, uint32_t replication);

    /// Read a DFS file to a local path.
    [[nodiscard]] Result<Void> get(std::string_view dfs_path, std::string_view local_path);

    /// Exact positional read bound to the latest published identity from stat().
    [[nodiscard]] Result<std::string> read_exact(std::string_view dfs_path,
                                                 uint64_t offset,
                                                 uint64_t length);

    /// Exact positional read bound to a caller-provided published identity.
    [[nodiscard]] Result<std::string> read_exact(std::string_view dfs_path,
                                                 uint64_t offset,
                                                 uint64_t length,
                                                 const FileIdentity& expected_identity);

    // Admin / diagnostic operations

    /// Cluster-wide summary (capacity, nodes, blocks, files).
    struct ClusterInfo {
        uint64_t total_capacity_bytes = 0;
        uint64_t used_bytes = 0;
        uint64_t free_bytes = 0;
        uint32_t live_datanodes = 0;
        uint32_t dead_datanodes = 0;
        uint64_t total_blocks = 0;
        uint64_t total_files = 0;
        uint64_t total_directories = 0;
        uint32_t under_replicated_blocks = 0;
    };
    [[nodiscard]] Result<ClusterInfo> get_cluster_info();

    /// Per-DataNode summary returned by list_datanodes / get_datanode_info.
    struct DataNodeSummary {
        uint64_t datanode_id = 0;
        std::string uuid;
        std::string hostname;
        std::string ip;
        uint32_t rpc_port = 0;
        uint32_t data_port = 0;
        std::string rack;
        std::string state;
        uint64_t capacity_bytes = 0;
        uint64_t used_bytes = 0;
        uint64_t free_bytes = 0;
        uint64_t last_heartbeat_ms = 0;
        uint32_t block_count = 0;
    };
    [[nodiscard]] Result<std::vector<DataNodeSummary>> list_datanodes(bool include_dead = false);
    [[nodiscard]] Result<DataNodeSummary> get_datanode_info(uint64_t datanode_id);
    [[nodiscard]] Result<DataNodeSummary> get_datanode_info_by_uuid(std::string_view uuid);

    /// Inode details (file or directory).
    struct InodeDetail {
        uint64_t inode_id = 0;
        std::string type;
        uint64_t parent_id = 0;
        std::string name;
        std::string owner;
        std::string group;
        uint32_t permission = 0;
        uint64_t length = 0;
        uint32_t replication = 0;
        uint64_t block_size = 0;
        std::string state;
        uint64_t ctime_ms = 0;
        uint64_t mtime_ms = 0;
        uint32_t block_count = 0;
        uint32_t child_count = 0;
    };
    [[nodiscard]] Result<InodeDetail> get_inode_info(uint64_t inode_id);
    [[nodiscard]] Result<InodeDetail> get_inode_info_by_path(std::string_view path);

    /// Block location info.
    struct BlockLocation {
        uint64_t datanode_id = 0;
        std::string host;
        uint32_t data_port = 0;
    };

    /// Per-block detail for file blocks listing.
    struct FileBlockDetail {
        uint64_t block_id = 0;
        uint32_t block_index = 0;
        uint64_t generation_stamp = 0;
        uint64_t length = 0;
        std::string state;
        uint32_t desired_replicas = 0;
        uint32_t actual_replicas = 0;
        std::vector<BlockLocation> locations;
    };
    [[nodiscard]] Result<std::vector<FileBlockDetail>> get_file_blocks(uint64_t inode_id);
    [[nodiscard]] Result<std::vector<FileBlockDetail>> get_file_blocks_by_path(
        std::string_view path);

    /// Detailed replica info for a single block.
    struct ReplicaDetail {
        uint64_t datanode_id = 0;
        std::string hostname;
        std::string state;
        uint64_t length = 0;
        uint64_t generation_stamp = 0;
        uint64_t report_time_ms = 0;
    };

    struct BlockDetail {
        uint64_t block_id = 0;
        uint64_t inode_id = 0;
        uint32_t block_index = 0;
        uint64_t generation_stamp = 0;
        uint64_t length = 0;
        std::string state;
        uint32_t desired_replicas = 0;
        std::vector<ReplicaDetail> replicas;
    };
    [[nodiscard]] Result<BlockDetail> get_block_info(uint64_t block_id);

private:
    explicit DfsClient(DfsClientConfig config);

    /// Initialize the brpc channel to NameNode.
    [[nodiscard]] bool init();

    /// Read one exact byte range from a located block with replica fallback.
    [[nodiscard]] Result<std::string> read_block(const LocatedBlock& block,
                                                 uint64_t offset,
                                                 uint64_t length);

    /// Generate a stable per-logical-call request ID.
    [[nodiscard]] std::string next_request_id();

    [[nodiscard]] Result<std::string> read_exact_with_status(std::string_view dfs_path,
                                                             uint64_t offset,
                                                             uint64_t length,
                                                             const FileStatus& fs,
                                                             const FileIdentity& expected_identity);

    [[nodiscard]] Result<DfsOutputStream> create_output_stream(std::string_view dfs_path,
                                                               bool overwrite,
                                                               FileAppendMode file_append_mode);

private:
    DfsClientConfig config_;
    brpc::Channel namenode_channel_;
    std::atomic<uint64_t> request_seq_{0};
};

} // namespace pl::minidfs
