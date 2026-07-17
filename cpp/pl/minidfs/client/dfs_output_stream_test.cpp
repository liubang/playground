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
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <netinet/in.h>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cpp/pl/minidfs/client/dfs_client.h"
#include "cpp/pl/minidfs/client/dfs_output_stream.h"
#include "cpp/pl/minidfs/common/block_token.h"
#include "cpp/pl/minidfs/common/checksum.h"
#include "cpp/pl/minidfs/common/error_code.h"
#include "cpp/pl/minidfs/common/time_util.h"
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
        allocated_client_ids.push_back(request->header().client_id());
        allocated_request_ids.push_back(request->header().request_id());

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
        completed_lengths.push_back(request->expected_length());
        completed_has_checksums.push_back(request->has_expected_checksum());
        completed_checksums.push_back(
            request->has_expected_checksum() ? request->expected_checksum() : 0u);
        response->mutable_status()->set_code(0);
        auto* identity = response->mutable_file_identity();
        identity->set_inode_id(request->inode_id());
        identity->set_content_generation(complete_count);
        identity->set_length(request->expected_length());
        identity->set_checksum(request->has_expected_checksum() ? request->expected_checksum()
                                                                : 0u);
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
    std::vector<std::string> allocated_request_ids;
    std::vector<std::string> renewed_client_ids;
    std::vector<std::string> completed_client_ids;
    std::vector<uint64_t> committed_lengths;
    std::vector<uint64_t> completed_lengths;
    std::vector<bool> completed_has_checksums;
    std::vector<uint32_t> completed_checksums;
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
        ASSERT_EQ(namenode_server_.AddService(static_cast<protocol::NameNodeService*>(&namenode_),
                                              brpc::SERVER_DOESNT_OWN_SERVICE),
                  0);
        ASSERT_EQ(
            namenode_server_.AddService(static_cast<protocol::DataNodeProtocolService*>(&namenode_),
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
                                              .initial_checksum = compute_crc32c(nullptr, 0),
                                          });
    ASSERT_TRUE(stream.hasValue());
    ASSERT_TRUE(stream.value().write("abcdefgh", 8).hasValue());
    ASSERT_TRUE(stream.value().close().hasValue());

    EXPECT_TRUE(stream.value().closed());
    EXPECT_EQ(stream.value().bytes_written(), 8u);
    EXPECT_EQ(namenode_.allocated_indices, (std::vector<uint32_t>{0, 1}));
    EXPECT_EQ(namenode_.allocated_client_ids, (std::vector<std::string>{"client-a", "client-a"}));
    ASSERT_EQ(namenode_.allocated_request_ids.size(), 2u);
    EXPECT_NE(namenode_.allocated_request_ids[0], namenode_.allocated_request_ids[1]);
    EXPECT_EQ(namenode_.committed_lengths, (std::vector<uint64_t>{5, 3}));
    EXPECT_EQ(namenode_.renew_count, 2u);
    EXPECT_EQ(namenode_.complete_count, 1u);
    ASSERT_EQ(namenode_.completed_lengths, (std::vector<uint64_t>{8}));
    ASSERT_EQ(namenode_.completed_checksums.size(), 1u);
    EXPECT_EQ(namenode_.completed_checksums.front(), compute_crc32c("abcdefgh"));
    ASSERT_TRUE(stream.value().published_identity().has_value());
    EXPECT_EQ(stream.value().published_identity()->inode_id, 42u);
    EXPECT_EQ(stream.value().published_identity()->content_generation, 1u);
    EXPECT_EQ(stream.value().published_identity()->length, 8u);
    EXPECT_EQ(stream.value().published_identity()->checksum, compute_crc32c("abcdefgh"));
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
                                              .initial_length = 4,
                                              .initial_checksum = compute_crc32c("base"),
                                          });
    ASSERT_TRUE(stream.hasValue());
    ASSERT_TRUE(stream.value().write("xy", 2).hasValue());
    ASSERT_TRUE(stream.value().close().hasValue());

    EXPECT_EQ(namenode_.allocated_indices, (std::vector<uint32_t>{3}));
    EXPECT_EQ(namenode_.allocated_client_ids, (std::vector<std::string>{"client-a"}));
    EXPECT_EQ(namenode_.committed_lengths, (std::vector<uint64_t>{2}));
    EXPECT_EQ(namenode_.completed_lengths, (std::vector<uint64_t>{6}));
    EXPECT_EQ(namenode_.completed_has_checksums, (std::vector<bool>{true}));
    EXPECT_EQ(namenode_.completed_checksums, (std::vector<uint32_t>{compute_crc32c("basexy")}));
    EXPECT_EQ(datanode_.blocks.at(1003), "xy");
}

TEST_F(DfsOutputStreamTest, RejectsAppendWithUnknownInitialChecksum) {
    auto stream = DfsOutputStream::create(&namenode_channel_,
                                          42,
                                          "client-a",
                                          {
                                              .block_size = 5,
                                              .chunk_size = 2,
                                              .replication = 1,
                                              .rpc_timeout_ms = 1000,
                                              .starting_block_index = 3,
                                              .initial_length = 4,
                                              .initial_checksum = std::nullopt,
                                          });
    ASSERT_TRUE(stream.hasError());
    EXPECT_TRUE(namenode_.completed_lengths.empty());
}

class FakeReadNameNode : public protocol::NameNodeService {
public:
    struct BlockData {
        uint64_t block_id = 0;
        uint64_t generation_stamp = 0;
        uint64_t offset = 0;
        std::string data;
    };

    void set_datanode_port(int port) { datanode_port_ = port; }

    void configure_file(std::string path,
                        FileIdentity identity,
                        std::string source_data,
                        uint64_t block_size) {
        path_ = std::move(path);
        identity_ = identity;
        const uint64_t visible_length = std::min<uint64_t>(identity_.length, source_data.size());
        source_data.resize(visible_length);
        blocks_.clear();
        uint64_t offset = 0;
        uint32_t index = 0;
        while (offset < source_data.size()) {
            const uint64_t chunk = std::min<uint64_t>(block_size, source_data.size() - offset);
            blocks_.push_back(BlockData{
                .block_id = 9000 + index,
                .generation_stamp = 100 + index,
                .offset = offset,
                .data = source_data.substr(offset, chunk),
            });
            offset += chunk;
            ++index;
        }
    }

    void set_identity(FileIdentity identity) { identity_ = identity; }

    const std::vector<BlockData>& blocks() const { return blocks_; }

    uint32_t get_located_blocks_calls() const { return get_located_blocks_calls_; }

    void reset_counters() { get_located_blocks_calls_ = 0; }

    void GetFileStatus(google::protobuf::RpcController* /*controller*/,
                       const protocol::GetFileStatusRequest* request,
                       protocol::GetFileStatusResponse* response,
                       google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
        if (request->path() != path_) {
            response->mutable_status()->set_code(static_cast<uint32_t>(ErrorCode::kFileNotFound));
            response->mutable_status()->set_message("file not found");
            return;
        }

        response->mutable_status()->set_code(0);
        auto* status = response->mutable_file_status();
        status->set_inode_id(identity_.inode_id);
        status->set_path(path_);
        status->set_is_dir(false);
        status->set_length(identity_.length);
        status->set_replication(1);
        status->set_block_size(4);
        status->set_mtime_ms(1);
        status->set_owner("u");
        status->set_group("g");
        status->set_permission(0644);
        status->set_file_append_mode(protocol::FILE_APPEND_MODE_APPENDABLE);
        auto* published = status->mutable_published_identity();
        published->set_inode_id(identity_.inode_id);
        published->set_content_generation(identity_.content_generation);
        published->set_length(identity_.length);
        published->set_checksum(identity_.checksum);
        published->set_checksum_valid(identity_.checksum_valid);
    }

    void GetLocatedBlocks(google::protobuf::RpcController* /*controller*/,
                          const protocol::GetLocatedBlocksRequest* request,
                          protocol::GetLocatedBlocksResponse* response,
                          google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
        ++get_located_blocks_calls_;
        if (request->inode_id() != identity_.inode_id) {
            response->mutable_status()->set_code(static_cast<uint32_t>(ErrorCode::kFileNotFound));
            response->mutable_status()->set_message("inode not found");
            return;
        }
        if (request->has_expected_file_identity()) {
            const auto& expected = request->expected_file_identity();
            const bool same =
                expected.inode_id() == identity_.inode_id &&
                expected.content_generation() == identity_.content_generation &&
                expected.length() == identity_.length &&
                expected.checksum_valid() == identity_.checksum_valid &&
                (!expected.checksum_valid() || expected.checksum() == identity_.checksum);
            if (!same) {
                response->mutable_status()->set_code(
                    static_cast<uint32_t>(ErrorCode::kInvalidArgument));
                response->mutable_status()->set_message("stale expected file identity");
                return;
            }
        }

        response->mutable_status()->set_code(0);
        auto* file_identity = response->mutable_file_identity();
        file_identity->set_inode_id(identity_.inode_id);
        file_identity->set_content_generation(identity_.content_generation);
        file_identity->set_length(identity_.length);
        file_identity->set_checksum(identity_.checksum);
        file_identity->set_checksum_valid(identity_.checksum_valid);

        for (size_t i = 0; i < blocks_.size(); ++i) {
            const auto& block = blocks_[i];
            auto* out = response->add_blocks();
            out->set_block_id(block.block_id);
            out->set_generation_stamp(block.generation_stamp);
            out->set_offset(block.offset);
            out->set_length(block.data.size());
            auto* location = out->add_locations();
            location->set_datanode_id(1);
            location->set_host("127.0.0.1");
            location->set_data_port(static_cast<uint32_t>(datanode_port_));

            auto* token = out->mutable_block_token();
            token->set_block_id(block.block_id);
            token->set_generation_stamp(block.generation_stamp);
            token->set_inode_id(identity_.inode_id);
            token->set_block_index(static_cast<uint32_t>(i));
            token->set_permissions(kBlockTokenPermissionRead);
            token->set_expires_at_ms(now_ms() + 60000);
            token->set_signature("fake-signature");
            auto* token_identity = token->mutable_file_identity();
            token_identity->set_inode_id(identity_.inode_id);
            token_identity->set_content_generation(identity_.content_generation);
            token_identity->set_length(identity_.length);
            token_identity->set_checksum(identity_.checksum);
            token_identity->set_checksum_valid(identity_.checksum_valid);
        }
    }

private:
    std::string path_;
    int datanode_port_ = 0;
    FileIdentity identity_;
    std::vector<BlockData> blocks_;
    uint32_t get_located_blocks_calls_ = 0;
};

class FakeReadDataNode : public protocol::DataTransferService {
public:
    void set_blocks(std::vector<FakeReadNameNode::BlockData> blocks) {
        blocks_.clear();
        for (auto& block : blocks) {
            blocks_.emplace(block.block_id, std::move(block.data));
        }
    }

    uint32_t read_block_calls() const { return read_block_calls_; }

    void ReadBlock(google::protobuf::RpcController* /*controller*/,
                   const protocol::ReadBlockRequest* request,
                   protocol::ReadBlockResponse* response,
                   google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
        ++read_block_calls_;

        if (!request->has_block_token() || !request->block_token().has_file_identity()) {
            response->mutable_status()->set_code(
                static_cast<uint32_t>(ErrorCode::kPermissionDenied));
            response->mutable_status()->set_message("missing identity-bound token");
            return;
        }

        auto it = blocks_.find(request->block_id());
        if (it == blocks_.end()) {
            response->mutable_status()->set_code(static_cast<uint32_t>(ErrorCode::kBlockNotFound));
            response->mutable_status()->set_message("block not found");
            return;
        }
        const auto& payload = it->second;
        if (request->offset() > payload.size() ||
            request->length() > payload.size() - request->offset()) {
            response->mutable_status()->set_code(
                static_cast<uint32_t>(ErrorCode::kInvalidArgument));
            response->mutable_status()->set_message("invalid range");
            return;
        }

        const std::string chunk = payload.substr(static_cast<size_t>(request->offset()),
                                                 static_cast<size_t>(request->length()));
        response->mutable_status()->set_code(0);
        response->set_data(chunk);
        response->set_length(chunk.size());
        response->set_checksum(compute_crc32c(chunk.data(), chunk.size()));
    }

private:
    std::unordered_map<uint64_t, std::string> blocks_;
    uint32_t read_block_calls_ = 0;
};

class DfsClientReadExactTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(datanode_server_.AddService(&datanode_, brpc::SERVER_DOESNT_OWN_SERVICE), 0);
        const int datanode_port = pick_unused_port();
        ASSERT_GT(datanode_port, 0);
        ASSERT_EQ(datanode_server_.Start(datanode_port, nullptr), 0);

        namenode_.set_datanode_port(datanode_server_.listen_address().port);
        ASSERT_EQ(namenode_server_.AddService(&namenode_, brpc::SERVER_DOESNT_OWN_SERVICE), 0);
        const int namenode_port = pick_unused_port();
        ASSERT_GT(namenode_port, 0);
        ASSERT_EQ(namenode_server_.Start(namenode_port, nullptr), 0);

        DfsClientConfig config;
        config.namenode_address =
            "127.0.0.1:" + std::to_string(namenode_server_.listen_address().port);
        config.rpc_timeout_ms = 1000;
        config.max_retry = 0;
        config.client_id = "test-client";
        client_ = DfsClient::create(config);
        ASSERT_NE(client_, nullptr);

        temp_dir_ =
            std::filesystem::temp_directory_path() /
            ("minidfs-client-read-" + std::to_string(::getpid()) + "-" + std::to_string(now_ms()));
        std::filesystem::create_directories(temp_dir_);
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(temp_dir_, ignored);
        namenode_server_.Stop(0);
        namenode_server_.Join();
        datanode_server_.Stop(0);
        datanode_server_.Join();
    }

    std::string read_local_file(const std::filesystem::path& path) {
        std::ifstream ifs(path, std::ios::binary);
        EXPECT_TRUE(ifs.is_open());
        return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    }

    FakeReadNameNode namenode_;
    FakeReadDataNode datanode_;
    brpc::Server namenode_server_;
    brpc::Server datanode_server_;
    std::unique_ptr<DfsClient> client_;
    std::filesystem::path temp_dir_;
};

TEST_F(DfsClientReadExactTest, TruncatedAppendableUnknownChecksumStillAllowsReadAndGet) {
    const std::string dfs_path = "/appendable.dat";
    const std::string source = "abcdefghij";
    FileIdentity identity{
        .inode_id = 7,
        .content_generation = 3,
        .length = 6,
        .checksum = 0x99999999u,
        .checksum_valid = false,
    };
    namenode_.configure_file(dfs_path, identity, source, 4);
    datanode_.set_blocks(namenode_.blocks());

    auto read_result = client_->read_exact(dfs_path, 0, identity.length, identity);
    ASSERT_TRUE(read_result.hasValue());
    EXPECT_EQ(read_result.value(), source.substr(0, identity.length));

    const auto local_path = temp_dir_ / "appendable.download";
    auto get_result = client_->get(dfs_path, local_path.string());
    ASSERT_TRUE(get_result.hasValue());
    EXPECT_EQ(read_local_file(local_path), source.substr(0, identity.length));
    EXPECT_GT(datanode_.read_block_calls(), 0u);
}

TEST_F(DfsClientReadExactTest, TruncatedAppendableIdentityChangeStillRejectedWhenChecksumUnknown) {
    const std::string dfs_path = "/appendable.dat";
    const std::string source = "abcdefghij";
    FileIdentity old_identity{
        .inode_id = 7,
        .content_generation = 3,
        .length = 6,
        .checksum = 0x11111111u,
        .checksum_valid = false,
    };
    namenode_.configure_file(dfs_path, old_identity, source, 4);
    datanode_.set_blocks(namenode_.blocks());

    FileIdentity new_identity = old_identity;
    new_identity.content_generation = 4;
    new_identity.checksum = 0x22222222u; // ignored when checksum_valid=false
    namenode_.set_identity(new_identity);
    namenode_.reset_counters();

    auto read_result = client_->read_exact(dfs_path, 0, old_identity.length, old_identity);
    ASSERT_TRUE(read_result.hasError());
    EXPECT_EQ(read_result.error().code(), static_cast<status_code_t>(ErrorCode::kInvalidArgument));
    EXPECT_EQ(namenode_.get_located_blocks_calls(), 0u);
}

} // namespace
} // namespace pl::minidfs
