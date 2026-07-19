// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.

#include <cstddef>
#include <exception>
#include <gflags/gflags.h>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "cpp/pl/minidfs/client/dfs_client.h"
#include "cpp/pl/minivessel/minidfs_filesystem.h"

DEFINE_string(namenode, "namenode:9000", "MiniDFS NameNode address");
DEFINE_string(dfs_root, "/minivessel-e2e", "Isolated DFS root for this test");

namespace pl::minivessel {
namespace {

void Require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::shared_ptr<minidfs::DfsClient> MakeClient() {
    minidfs::DfsClientConfig config;
    config.namenode_address = FLAGS_namenode;
    config.rpc_timeout_ms = 10'000;
    config.max_retry = 3;
    config.client_id = "minivessel-immutable-object-e2e";
    auto client = minidfs::DfsClient::create(std::move(config));
    Require(client != nullptr, "failed to construct MiniDFS client");
    return std::shared_ptr<minidfs::DfsClient>(std::move(client));
}

void RunImmutableObjectE2E() {
    MiniDfsFileSystem backend(MakeClient());
    Require(backend.capabilities().has(ObjectStorageFeature::kImmutableObjects),
            "MiniDFS adapter did not advertise immutable objects");
    auto filesystem = backend.object_filesystem();
    Require(filesystem != nullptr, "MiniDFS object filesystem is null");

    const std::string path = FLAGS_dfs_root + "/checkpoint.bin";
    const std::string payload = "immutable-checkpoint-payload";
    auto handle = filesystem->create(path, {.overwrite = true});
    Require(handle.ok(), handle.status().ToString());
    auto status =
        filesystem->append(*handle, std::as_bytes(std::span(payload.data(), payload.size())));
    Require(status.ok(), status.ToString());
    auto identity = filesystem->close(*handle);
    Require(identity.ok(), identity.status().ToString());
    Require(identity->file_id != 0 && identity->checksum_valid,
            "published immutable object lacks identity");

    auto reader = filesystem->open(path, *identity);
    Require(reader.ok(), reader.status().ToString());
    std::vector<std::byte> bytes(payload.size());
    status = filesystem->read_at(*reader, 0, bytes);
    Require(status.ok(), status.ToString());
    Require(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()) == payload,
            "immutable object payload mismatch");
    auto closed = filesystem->close(*reader);
    Require(closed.ok() && *closed == *identity, "immutable object identity changed on read");

    status = filesystem->remove(path, *identity);
    Require(status.ok(), status.ToString());
}

} // namespace
} // namespace pl::minivessel

int main(int argc, char** argv) {
    gflags::SetUsageMessage("MiniVessel immutable-object E2E against a live MiniDFS cluster");
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    try {
        pl::minivessel::RunImmutableObjectE2E();
        std::cout << "[PASS] MiniVessel MiniDFS immutable-object E2E passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] MiniVessel MiniDFS immutable-object E2E: " << error.what() << '\n';
        return 1;
    }
}
