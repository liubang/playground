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
// Created: 2026/05/10 23:30

#include "cpp/pl/ascii_table/pretty.h"
#include "cpp/pl/minidfs/client/dfs_client.h"
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fmt/core.h>
#include <gflags/gflags.h>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

DEFINE_string(namenode, "127.0.0.1:8020", "NameNode address (host:port)");
DEFINE_int32(rpc_timeout_ms, 5000, "RPC timeout in milliseconds");
DEFINE_int32(replication, 3, "Default replication factor");
DEFINE_int64(block_size, 128 * 1024 * 1024, "Default block size in bytes");

namespace {

using pl::minidfs::DfsClient;
using pl::minidfs::DfsClientConfig;
using pl::minidfs::FileStatus;

/// Generate a unique client_id.
std::string generate_client_id() {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return fmt::format("cli-{}-{}", ::getpid(), now);
}

/// Format timestamp (ms) as human-readable string.
std::string format_time(uint64_t mtime_ms) {
    if (mtime_ms == 0) {
        return "-";
    }
    auto secs = static_cast<time_t>(mtime_ms / 1000);
    std::tm tm{};
    ::localtime_r(&secs, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M");
    return oss.str();
}

/// Format file size in human-readable form.
std::string format_size(uint64_t size) {
    if (size == 0) {
        return "0";
    }
    constexpr uint64_t KB = 1024;
    constexpr uint64_t MB = KB * 1024;
    constexpr uint64_t GB = MB * 1024;
    if (size >= GB) {
        return fmt::format("{:.1f}G", static_cast<double>(size) / static_cast<double>(GB));
    }
    if (size >= MB) {
        return fmt::format("{:.1f}M", static_cast<double>(size) / static_cast<double>(MB));
    }
    if (size >= KB) {
        return fmt::format("{:.1f}K", static_cast<double>(size) / static_cast<double>(KB));
    }
    return fmt::format("{}B", size);
}

/// Format permission as octal string.
std::string format_permission(uint32_t perm) { return fmt::format("{:04o}", perm); }

/// Create the DfsClient from flags.
std::unique_ptr<DfsClient> make_client() {
    DfsClientConfig config;
    config.namenode_address = FLAGS_namenode;
    config.rpc_timeout_ms = FLAGS_rpc_timeout_ms;
    config.max_retry = 3;
    config.client_id = generate_client_id();
    config.replication = static_cast<uint32_t>(FLAGS_replication);
    config.block_size = static_cast<uint64_t>(FLAGS_block_size);
    return DfsClient::create(std::move(config));
}

int cmd_mkdir(DfsClient* client, int argc, char* argv[]) {
    if (argc < 1) {
        std::cerr << "Usage: minidfs -mkdir <path>\n";
        return 1;
    }
    auto result = client->mkdir(argv[0]);
    if (result.hasError()) {
        std::cerr << "mkdir failed: " << result.error().describe() << "\n";
        return 1;
    }
    return 0;
}

int cmd_ls(DfsClient* client, int argc, char* argv[]) {
    std::string path = (argc > 0) ? argv[0] : "/";
    auto result = client->ls(path);
    if (result.hasError()) {
        std::cerr << "ls failed: " << result.error().describe() << "\n";
        return 1;
    }

    const auto& entries = result.value();
    if (entries.empty()) {
        std::cout << "(empty directory)\n";
        return 0;
    }

    pl::pretty::Pretty table(
        {"Type", "Perm", "Repl", "Owner", "Group", "Size", "Modified", "Name"});
    for (const auto& entry : entries) {
        table.add_row({
            entry.is_dir ? "d" : "-",
            format_permission(entry.permission),
            std::to_string(entry.replication),
            entry.owner,
            entry.group,
            entry.is_dir ? "-" : format_size(entry.length),
            format_time(entry.mtime_ms),
            entry.path,
        });
    }
    table.render();
    return 0;
}

int cmd_stat(DfsClient* client, int argc, char* argv[]) {
    if (argc < 1) {
        std::cerr << "Usage: minidfs -stat <path>\n";
        return 1;
    }
    auto result = client->stat(argv[0]);
    if (result.hasError()) {
        std::cerr << "stat failed: " << result.error().describe() << "\n";
        return 1;
    }

    const auto& fs = result.value();
    pl::pretty::Pretty table({"Property", "Value"});
    table.add_row({"Path", fs.path});
    table.add_row({"Type", fs.is_dir ? "directory" : "file"});
    table.add_row({"Inode ID", std::to_string(fs.inode_id)});
    table.add_row({"Size", format_size(fs.length)});
    table.add_row({"Replication", std::to_string(fs.replication)});
    table.add_row({"Block Size", format_size(fs.block_size)});
    table.add_row({"Owner", fs.owner});
    table.add_row({"Group", fs.group});
    table.add_row({"Permission", format_permission(fs.permission)});
    table.add_row({"Modified", format_time(fs.mtime_ms)});
    table.render();
    return 0;
}

int cmd_rm(DfsClient* client, int argc, char* argv[]) {
    if (argc < 1) {
        std::cerr << "Usage: minidfs -rm [-r] <path>\n";
        return 1;
    }

    bool recursive = false;
    int path_idx = 0;
    if (std::string(argv[0]) == "-r" || std::string(argv[0]) == "-R") {
        recursive = true;
        path_idx = 1;
    }
    if (path_idx >= argc) {
        std::cerr << "Usage: minidfs -rm [-r] <path>\n";
        return 1;
    }

    auto result = client->rm(argv[path_idx], recursive);
    if (result.hasError()) {
        std::cerr << "rm failed: " << result.error().describe() << "\n";
        return 1;
    }
    return 0;
}

int cmd_mv(DfsClient* client, int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: minidfs -mv <src> <dst>\n";
        return 1;
    }
    auto result = client->mv(argv[0], argv[1]);
    if (result.hasError()) {
        std::cerr << "mv failed: " << result.error().describe() << "\n";
        return 1;
    }
    return 0;
}

int cmd_put(DfsClient* client, int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: minidfs -put <local_path> <dfs_path>\n";
        return 1;
    }
    auto result = client->put(argv[0], argv[1]);
    if (result.hasError()) {
        std::cerr << "put failed: " << result.error().describe() << "\n";
        return 1;
    }
    std::cout << "Uploaded " << argv[0] << " -> " << argv[1] << "\n";
    return 0;
}

int cmd_get(DfsClient* client, int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: minidfs -get <dfs_path> <local_path>\n";
        return 1;
    }
    auto result = client->get(argv[0], argv[1]);
    if (result.hasError()) {
        std::cerr << "get failed: " << result.error().describe() << "\n";
        return 1;
    }
    std::cout << "Downloaded " << argv[0] << " -> " << argv[1] << "\n";
    return 0;
}

void print_usage() {
    std::cerr << "Usage: minidfs [options] <command> [args...]\n"
              << "\n"
              << "Commands:\n"
              << "  -mkdir <path>              Create a directory\n"
              << "  -ls [path]                 List directory contents\n"
              << "  -stat <path>               Show file/directory status\n"
              << "  -rm [-r] <path>            Delete a file or directory\n"
              << "  -mv <src> <dst>            Rename/move a file or directory\n"
              << "  -put <local> <dfs_path>    Upload a local file to DFS\n"
              << "  -get <dfs_path> <local>    Download a DFS file to local\n"
              << "\n"
              << "Options:\n"
              << "  -namenode=<host:port>      NameNode address (default: 127.0.0.1:8020)\n"
              << "  -rpc_timeout_ms=<ms>       RPC timeout (default: 5000)\n"
              << "  -replication=<n>           Replication factor (default: 3)\n"
              << "  -block_size=<bytes>        Block size (default: 128MB)\n";
}

} // namespace

int main(int argc, char* argv[]) {
    gflags::SetUsageMessage("MiniDFS command-line client");
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    if (argc < 2) {
        print_usage();
        return 1;
    }

    auto client = make_client();
    if (!client) {
        std::cerr << "Failed to connect to NameNode at " << FLAGS_namenode << "\n";
        return 1;
    }

    std::string cmd = argv[1];

    // Remaining args after the command
    int cmd_argc = argc - 2;
    char** cmd_argv = argv + 2;

    if (cmd == "-mkdir" || cmd == "mkdir") {
        return cmd_mkdir(client.get(), cmd_argc, cmd_argv);
    }
    if (cmd == "-ls" || cmd == "ls") {
        return cmd_ls(client.get(), cmd_argc, cmd_argv);
    }
    if (cmd == "-stat" || cmd == "stat") {
        return cmd_stat(client.get(), cmd_argc, cmd_argv);
    }
    if (cmd == "-rm" || cmd == "rm") {
        return cmd_rm(client.get(), cmd_argc, cmd_argv);
    }
    if (cmd == "-mv" || cmd == "mv") {
        return cmd_mv(client.get(), cmd_argc, cmd_argv);
    }
    if (cmd == "-put" || cmd == "put") {
        return cmd_put(client.get(), cmd_argc, cmd_argv);
    }
    if (cmd == "-get" || cmd == "get") {
        return cmd_get(client.get(), cmd_argc, cmd_argv);
    }

    std::cerr << "Unknown command: " << cmd << "\n";
    print_usage();
    return 1;
}
