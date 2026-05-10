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

#include "cpp/pl/minidfs/common/types.h"
#include "cpp/pl/status/result.h"
#include <brpc/channel.h>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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

    // =========================================================================
    // Namespace operations
    // =========================================================================

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

    // =========================================================================
    // File read/write operations
    // =========================================================================

    /// Write a local file to DFS using pipeline replication.
    /// The full path must not already exist.
    [[nodiscard]] Result<Void> put(std::string_view local_path, std::string_view dfs_path);

    /// Read a DFS file to a local path.
    [[nodiscard]] Result<Void> get(std::string_view dfs_path, std::string_view local_path);

private:
    explicit DfsClient(DfsClientConfig config);

    /// Initialize the brpc channel to NameNode.
    [[nodiscard]] bool init();

    /// Write one block through the pipeline. Returns bytes written.
    [[nodiscard]] Result<uint64_t> write_block(uint64_t inode_id,
                                               uint32_t block_index,
                                               const char* data,
                                               uint64_t length);

    /// Read one block from a DataNode.
    [[nodiscard]] Result<std::string> read_block(const LocatedBlock& block);

private:
    DfsClientConfig config_;
    brpc::Channel namenode_channel_;
};

} // namespace pl::minidfs
