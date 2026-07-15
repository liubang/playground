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
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gflags/gflags.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "cpp/pl/minidfs/client/dfs_client.h"

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
        client_ = DfsClient::create(std::move(config));
        require(client_ != nullptr, "DfsClient::create returned nullptr");
    }

    void run() {
        cleanup();
        try {
            verify_cluster_and_datanodes();
            verify_namespace();
            verify_file_sizes_and_diagnostics();
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
    std::unique_ptr<DfsClient> client_;
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
