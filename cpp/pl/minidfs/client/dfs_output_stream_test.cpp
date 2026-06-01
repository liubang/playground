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

#include <brpc/closure_guard.h>
#include <brpc/server.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <gtest/gtest.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cpp/pl/minidfs/client/dfs_output_stream.h"
#include "cpp/pl/minidfs/protocol/minidfs.pb.h"

namespace pl::minidfs {
namespace {

int pick_unused_port() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    socklen_t address_len = sizeof(address);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &address_len) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return ntohs(address.sin_port);
}

class FakeNameNode : public protocol::NameNodeService, public protocol::DataNodeProtocolService {
public:
    void set_datanode_port(int port) { datanode_port_ = port; }

    void AllocateBlock(google::protobuf::RpcController* /*controller*/,
                       const protocol::AllocateBlockRequest* request,
                       protocol::AllocateBlockResponse* response,
                       google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
        allocated_indices.push_back(request->block_index());
        allocated_client_ids.push_back(request->client_id());

        response->mutable_status()->set_code(0);
        auto* block = response->mutable_block();
        block->set_block_id(1000 + request->block_index());
        block->set_generation_stamp(2000 + request->block_index());
        auto* location = block->add_locations();
        location->set_datanode_id(1);
        location->set_host("127.0.0.1");
        location->set_data_port(static_cast<uint32_t>(datanode_port_));
    }

    void RenewLease(google::protobuf::RpcController* /*controller*/,
                    const protocol::RenewLeaseRequest* request,
                    protocol::RenewLeaseResponse* response,
                    google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
        ++renew_count;
        renewed_client_ids.push_back(request->client_id());
        response->mutable_status()->set_code(0);
    }

    void CompleteFile(google::protobuf::RpcController* /*controller*/,
                      const protocol::CompleteFileRequest* request,
                      protocol::CompleteFileResponse* response,
                      google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
        ++complete_count;
        completed_client_ids.push_back(request->client_id());
        response->mutable_status()->set_code(0);
    }

    void CommitBlock(google::protobuf::RpcController* /*controller*/,
                     const protocol::CommitBlockRequest* request,
                     protocol::CommitBlockResponse* response,
                     google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
        committed_lengths.push_back(request->length());
        response->mutable_status()->set_code(0);
    }

    std::vector<uint32_t> allocated_indices;
    std::vector<std::string> allocated_client_ids;
    std::vector<std::string> renewed_client_ids;
    std::vector<std::string> completed_client_ids;
    std::vector<uint64_t> committed_lengths;
    uint32_t renew_count = 0;
    uint32_t complete_count = 0;

private:
    int datanode_port_ = 0;
};

class FakeDataNode : public protocol::DataTransferService {
public:
    void WriteBlock(google::protobuf::RpcController* /*controller*/,
                    const protocol::WriteBlockRequest* request,
                    protocol::WriteBlockResponse* response,
                    google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
        blocks[request->block_id()].append(request->data());
        response->mutable_status()->set_code(0);
    }

    std::unordered_map<uint64_t, std::string> blocks;
};

class DfsOutputStreamTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(datanode_server_.AddService(&datanode_, brpc::SERVER_DOESNT_OWN_SERVICE), 0);
        const int datanode_port = pick_unused_port();
        ASSERT_GT(datanode_port, 0);
        ASSERT_EQ(datanode_server_.Start(datanode_port, nullptr), 0);

        namenode_.set_datanode_port(datanode_server_.listen_address().port);
        ASSERT_EQ(namenode_server_.AddService(
                      static_cast<protocol::NameNodeService*>(&namenode_),
                      brpc::SERVER_DOESNT_OWN_SERVICE),
                  0);
        ASSERT_EQ(namenode_server_.AddService(
                      static_cast<protocol::DataNodeProtocolService*>(&namenode_),
                      brpc::SERVER_DOESNT_OWN_SERVICE),
                  0);
        const int namenode_port = pick_unused_port();
        ASSERT_GT(namenode_port, 0);
        ASSERT_EQ(namenode_server_.Start(namenode_port, nullptr), 0);

        brpc::ChannelOptions options;
        options.timeout_ms = 1000;
        options.max_retry = 0;
        const std::string address =
            "127.0.0.1:" + std::to_string(namenode_server_.listen_address().port);
        ASSERT_EQ(namenode_channel_.Init(address.c_str(), &options), 0);
    }

    void TearDown() override {
        namenode_server_.Stop(0);
        namenode_server_.Join();
        datanode_server_.Stop(0);
        datanode_server_.Join();
    }

    FakeNameNode namenode_;
    FakeDataNode datanode_;
    brpc::Server namenode_server_;
    brpc::Server datanode_server_;
    brpc::Channel namenode_channel_;
};

TEST_F(DfsOutputStreamTest, WritesMultipleBlocksAndCompletes) {
    auto stream = DfsOutputStream::create(&namenode_channel_,
                                          42,
                                          "client-a",
                                          {
                                              .block_size = 5,
                                              .chunk_size = 2,
                                              .replication = 1,
                                              .rpc_timeout_ms = 1000,
                                          });
    ASSERT_TRUE(stream.hasValue());
    ASSERT_TRUE(stream.value().write("abcdefgh", 8).hasValue());
    ASSERT_TRUE(stream.value().close().hasValue());

    EXPECT_TRUE(stream.value().closed());
    EXPECT_EQ(stream.value().bytes_written(), 8u);
    EXPECT_EQ(namenode_.allocated_indices, (std::vector<uint32_t>{0, 1}));
    EXPECT_EQ(namenode_.committed_lengths, (std::vector<uint64_t>{5, 3}));
    EXPECT_EQ(namenode_.renew_count, 2u);
    EXPECT_EQ(namenode_.complete_count, 1u);
    EXPECT_EQ(datanode_.blocks.at(1000), "abcde");
    EXPECT_EQ(datanode_.blocks.at(1001), "fgh");
}

TEST_F(DfsOutputStreamTest, AppendStartsAtRequestedBlockIndex) {
    auto stream = DfsOutputStream::create(&namenode_channel_,
                                          42,
                                          "client-a",
                                          {
                                              .block_size = 5,
                                              .chunk_size = 2,
                                              .replication = 1,
                                              .rpc_timeout_ms = 1000,
                                              .starting_block_index = 3,
                                          });
    ASSERT_TRUE(stream.hasValue());
    ASSERT_TRUE(stream.value().write("xy", 2).hasValue());
    ASSERT_TRUE(stream.value().close().hasValue());

    EXPECT_EQ(namenode_.allocated_indices, (std::vector<uint32_t>{3}));
    EXPECT_EQ(namenode_.committed_lengths, (std::vector<uint64_t>{2}));
    EXPECT_EQ(datanode_.blocks.at(1003), "xy");
}

} // namespace
} // namespace pl::minidfs
// Created: 2026/06/01 23:29
