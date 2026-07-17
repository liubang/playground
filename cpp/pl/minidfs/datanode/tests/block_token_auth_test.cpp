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
// Created: 2026/07/16

// Unit tests for DataNode data-plane block token authorization.
// Verifies that DataTransferServiceImpl correctly validates HMAC-SHA256 block tokens
// for WriteBlock, ReadBlock, TransferBlock, and TruncateBlock operations.

#include <brpc/channel.h>
#include <brpc/controller.h>
#include <brpc/server.h>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <optional>
#include <string>

#include "cpp/pl/minidfs/common/block_token.h"
#include "cpp/pl/minidfs/common/error_code.h"
#include "cpp/pl/minidfs/datanode/block_reporter.h"
#include "cpp/pl/minidfs/datanode/data_transfer_service_impl.h"
#include "cpp/pl/minidfs/datanode/local_block_store.h"
#include "cpp/pl/minidfs/protocol/minidfs.pb.h"

namespace pl::minidfs {
namespace {

namespace fs = std::filesystem;

constexpr uint64_t kBlockId = 100;
constexpr uint64_t kGenerationStamp = 1000;
constexpr uint64_t kInodeId = 42;
constexpr uint32_t kBlockIndex = 0;
constexpr const char* kSecret = "test-block-token-secret-for-auth-test";

/// Fixture that sets up a LocalBlockStore, a DataTransferServiceImpl registered
/// on a brpc::Server, and a client stub for making RPC calls.
class BlockTokenAuthTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ =
            fs::temp_directory_path() / ("minidfs_token_auth_test_" + std::to_string(::getpid()) +
                                         "_" + std::to_string(counter_++));
        fs::create_directories(test_dir_);

        LocalBlockStore::Config config;
        config.storage_root = test_dir_.string();
        config.reserved_bytes = 0;
        store_ = std::make_unique<LocalBlockStore>(std::move(config));
        ASSERT_TRUE(store_->init().hasValue());

        // BlockReporter with no-op functions (we don't need real NN communication for auth tests)
        pl::minidfs::BlockReportFunc report_func =
            [](uint64_t,
               const pl::minidfs::BlockReport&) -> pl::Result<pl::minidfs::BlockReportResponse> {
            return pl::minidfs::BlockReportResponse{};
        };
        pl::minidfs::DeleteBlockFunc delete_func = [](uint64_t, uint64_t) -> pl::Result<pl::Void> {
            return pl::Void{};
        };

        BlockReporter::Config br_config{
            .datanode_id = 1,
            .full_report_interval_ms = 600000,
        };
        reporter_ =
            std::make_unique<BlockReporter>(br_config, store_.get(), report_func, delete_func);

        service_ =
            std::make_unique<DataTransferServiceImpl>(store_.get(), reporter_.get(), kSecret);

        // Start brpc server
        ASSERT_EQ(server_.AddService(service_.get(), brpc::SERVER_DOESNT_OWN_SERVICE), 0);
        brpc::ServerOptions options;
        ASSERT_EQ(server_.Start(0, &options), 0);
        port_ = server_.listen_address().port;

        // Create client channel
        brpc::ChannelOptions channel_opts;
        channel_opts.timeout_ms = 3000;
        ASSERT_EQ(channel_.Init("127.0.0.1", port_, &channel_opts), 0);
        stub_ = std::make_unique<protocol::DataTransferService_Stub>(&channel_);
    }

    void TearDown() override {
        server_.Stop(0);
        server_.Join();
        service_.reset();
        reporter_.reset();
        store_.reset();
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    /// Helper: issue a block token with the given permissions and a far-future expiry.
    static protocol::BlockTokenProto issue_token(uint32_t permissions,
                                                 uint64_t block_id = kBlockId) {
        return issue_block_token(block_id,
                                 kGenerationStamp,
                                 kInodeId,
                                 kBlockIndex,
                                 permissions,
                                 default_block_token_ttl_ms(),
                                 kSecret);
    }

    /// Helper: issue a token with a custom secret (simulates wrong secret).
    static protocol::BlockTokenProto issue_token_wrong_secret(uint32_t permissions) {
        return issue_block_token(kBlockId,
                                 kGenerationStamp,
                                 kInodeId,
                                 kBlockIndex,
                                 permissions,
                                 default_block_token_ttl_ms(),
                                 "wrong-secret");
    }

    /// Helper: issue an expired token.
    static protocol::BlockTokenProto issue_expired_token(uint32_t permissions) {
        // Set expiry far in the past
        protocol::BlockTokenProto token;
        token.set_block_id(kBlockId);
        token.set_generation_stamp(kGenerationStamp);
        token.set_inode_id(kInodeId);
        token.set_block_index(kBlockIndex);
        token.set_permissions(permissions);
        token.set_expires_at_ms(1); // epoch + 1ms = long expired
        token.set_signature(block_token_signature(
            kSecret, kBlockId, kGenerationStamp, kInodeId, kBlockIndex, permissions, 1));
        return token;
    }

    fs::path test_dir_;
    std::unique_ptr<LocalBlockStore> store_;
    std::unique_ptr<BlockReporter> reporter_;
    std::unique_ptr<DataTransferServiceImpl> service_;
    brpc::Server server_;
    int port_ = 0;
    brpc::Channel channel_;
    std::unique_ptr<protocol::DataTransferService_Stub> stub_;
    static inline int counter_ = 0;
};

// ========== ReadBlock Authorization Tests ==========

TEST_F(BlockTokenAuthTest, ReadBlockWithValidTokenSucceeds) {
    // First, create and finalize a block so it can be read
    ASSERT_TRUE(store_->create_block(kBlockId, kInodeId, kBlockIndex, kGenerationStamp).hasValue());
    std::string data = "hello world";
    ASSERT_TRUE(
        store_->append_chunk(kBlockId, kGenerationStamp, data.data(), data.size(), 0).hasValue());
    ASSERT_TRUE(store_->finalize_block(kBlockId, kGenerationStamp).hasValue());

    auto token = issue_token(kBlockTokenPermissionRead);

    brpc::Controller cntl;
    protocol::ReadBlockRequest req;
    req.set_block_id(kBlockId);
    req.set_generation_stamp(kGenerationStamp);
    req.set_offset(0);
    req.set_length(0);
    *req.mutable_block_token() = token;

    protocol::ReadBlockResponse resp;
    stub_->ReadBlock(&cntl, &req, &resp, nullptr);

    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    EXPECT_EQ(resp.status().code(), 0);
    EXPECT_EQ(resp.data(), data);
}

TEST_F(BlockTokenAuthTest, ReadBlockWithoutTokenFails) {
    // Create a block to read
    ASSERT_TRUE(store_->create_block(kBlockId, kInodeId, kBlockIndex, kGenerationStamp).hasValue());
    std::string data = "test";
    ASSERT_TRUE(
        store_->append_chunk(kBlockId, kGenerationStamp, data.data(), data.size(), 0).hasValue());
    ASSERT_TRUE(store_->finalize_block(kBlockId, kGenerationStamp).hasValue());

    brpc::Controller cntl;
    protocol::ReadBlockRequest req;
    req.set_block_id(kBlockId);
    req.set_generation_stamp(kGenerationStamp);
    req.set_offset(0);
    req.set_length(0);
    // No block_token set → empty/default BlockTokenProto

    protocol::ReadBlockResponse resp;
    stub_->ReadBlock(&cntl, &req, &resp, nullptr);

    ASSERT_FALSE(cntl.Failed());
    EXPECT_EQ(resp.status().code(), static_cast<uint32_t>(ErrorCode::kInvalidBlockToken));
}

TEST_F(BlockTokenAuthTest, ReadBlockWithWrongPermissionFails) {
    // Create a block to read
    ASSERT_TRUE(store_->create_block(kBlockId, kInodeId, kBlockIndex, kGenerationStamp).hasValue());
    std::string data = "test";
    ASSERT_TRUE(
        store_->append_chunk(kBlockId, kGenerationStamp, data.data(), data.size(), 0).hasValue());
    ASSERT_TRUE(store_->finalize_block(kBlockId, kGenerationStamp).hasValue());

    // Issue a Write-only token, try to Read
    auto token = issue_token(kBlockTokenPermissionWrite);

    brpc::Controller cntl;
    protocol::ReadBlockRequest req;
    req.set_block_id(kBlockId);
    req.set_generation_stamp(kGenerationStamp);
    req.set_offset(0);
    req.set_length(0);
    *req.mutable_block_token() = token;

    protocol::ReadBlockResponse resp;
    stub_->ReadBlock(&cntl, &req, &resp, nullptr);

    ASSERT_FALSE(cntl.Failed());
    EXPECT_EQ(resp.status().code(), static_cast<uint32_t>(ErrorCode::kInvalidBlockToken));
}

TEST_F(BlockTokenAuthTest, ReadBlockWithWrongSecretFails) {
    // Create a block to read
    ASSERT_TRUE(store_->create_block(kBlockId, kInodeId, kBlockIndex, kGenerationStamp).hasValue());
    std::string data = "test";
    ASSERT_TRUE(
        store_->append_chunk(kBlockId, kGenerationStamp, data.data(), data.size(), 0).hasValue());
    ASSERT_TRUE(store_->finalize_block(kBlockId, kGenerationStamp).hasValue());

    auto token = issue_token_wrong_secret(kBlockTokenPermissionRead);

    brpc::Controller cntl;
    protocol::ReadBlockRequest req;
    req.set_block_id(kBlockId);
    req.set_generation_stamp(kGenerationStamp);
    req.set_offset(0);
    req.set_length(0);
    *req.mutable_block_token() = token;

    protocol::ReadBlockResponse resp;
    stub_->ReadBlock(&cntl, &req, &resp, nullptr);

    ASSERT_FALSE(cntl.Failed());
    EXPECT_EQ(resp.status().code(), static_cast<uint32_t>(ErrorCode::kInvalidBlockToken));
}

TEST_F(BlockTokenAuthTest, ReadBlockWithExpiredTokenFails) {
    // Create a block to read
    ASSERT_TRUE(store_->create_block(kBlockId, kInodeId, kBlockIndex, kGenerationStamp).hasValue());
    std::string data = "test";
    ASSERT_TRUE(
        store_->append_chunk(kBlockId, kGenerationStamp, data.data(), data.size(), 0).hasValue());
    ASSERT_TRUE(store_->finalize_block(kBlockId, kGenerationStamp).hasValue());

    auto token = issue_expired_token(kBlockTokenPermissionRead);

    brpc::Controller cntl;
    protocol::ReadBlockRequest req;
    req.set_block_id(kBlockId);
    req.set_generation_stamp(kGenerationStamp);
    req.set_offset(0);
    req.set_length(0);
    *req.mutable_block_token() = token;

    protocol::ReadBlockResponse resp;
    stub_->ReadBlock(&cntl, &req, &resp, nullptr);

    ASSERT_FALSE(cntl.Failed());
    EXPECT_EQ(resp.status().code(), static_cast<uint32_t>(ErrorCode::kInvalidBlockToken));
}

TEST_F(BlockTokenAuthTest, ReadBlockWithWrongBlockIdFails) {
    // Create a block
    ASSERT_TRUE(store_->create_block(kBlockId, kInodeId, kBlockIndex, kGenerationStamp).hasValue());
    std::string data = "test";
    ASSERT_TRUE(
        store_->append_chunk(kBlockId, kGenerationStamp, data.data(), data.size(), 0).hasValue());
    ASSERT_TRUE(store_->finalize_block(kBlockId, kGenerationStamp).hasValue());

    // Token is for block_id=kBlockId, but request asks for block_id=999
    auto token = issue_token(kBlockTokenPermissionRead);

    brpc::Controller cntl;
    protocol::ReadBlockRequest req;
    req.set_block_id(999); // different from token's block_id
    req.set_generation_stamp(kGenerationStamp);
    req.set_offset(0);
    req.set_length(0);
    *req.mutable_block_token() = token;

    protocol::ReadBlockResponse resp;
    stub_->ReadBlock(&cntl, &req, &resp, nullptr);

    ASSERT_FALSE(cntl.Failed());
    EXPECT_EQ(resp.status().code(), static_cast<uint32_t>(ErrorCode::kInvalidBlockToken));
}

// ========== WriteBlock Authorization Tests ==========

TEST_F(BlockTokenAuthTest, WriteBlockWithValidTokenSucceeds) {
    auto token = issue_token(kBlockTokenPermissionWrite);

    brpc::Controller cntl;
    protocol::WriteBlockRequest req;
    req.set_block_id(kBlockId);
    req.set_inode_id(kInodeId);
    req.set_block_index(kBlockIndex);
    req.set_generation_stamp(kGenerationStamp);
    req.set_data("hello");
    req.set_chunk_index(0);
    req.set_checksum(compute_crc32c("hello", 5));
    req.set_is_last_chunk(true);
    *req.mutable_block_token() = token;

    protocol::WriteBlockResponse resp;
    stub_->WriteBlock(&cntl, &req, &resp, nullptr);

    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    EXPECT_EQ(resp.status().code(), 0);
    EXPECT_EQ(resp.ack_status(), static_cast<uint32_t>(AckStatus::kSuccess));
}

TEST_F(BlockTokenAuthTest, WriteBlockRejectsTokenBoundToDifferentInodeOrIndex) {
    auto token = issue_token(kBlockTokenPermissionWrite);

    protocol::WriteBlockRequest req;
    req.set_block_id(kBlockId);
    req.set_inode_id(kInodeId + 1);
    req.set_block_index(kBlockIndex);
    req.set_generation_stamp(kGenerationStamp);
    req.set_data("hello");
    req.set_chunk_index(0);
    req.set_checksum(compute_crc32c("hello", 5));
    req.set_is_last_chunk(true);
    *req.mutable_block_token() = token;

    brpc::Controller cntl;
    protocol::WriteBlockResponse resp;
    stub_->WriteBlock(&cntl, &req, &resp, nullptr);
    EXPECT_EQ(resp.status().code(), static_cast<uint32_t>(ErrorCode::kInvalidBlockToken));

    req.set_inode_id(kInodeId);
    req.set_block_index(kBlockIndex + 1);
    resp.Clear();
    cntl.Reset();
    stub_->WriteBlock(&cntl, &req, &resp, nullptr);
    EXPECT_EQ(resp.status().code(), static_cast<uint32_t>(ErrorCode::kInvalidBlockToken));
}

TEST_F(BlockTokenAuthTest, WriteBlockWithoutTokenFails) {
    brpc::Controller cntl;
    protocol::WriteBlockRequest req;
    req.set_block_id(kBlockId);
    req.set_inode_id(kInodeId);
    req.set_block_index(kBlockIndex);
    req.set_generation_stamp(kGenerationStamp);
    req.set_data("hello");
    req.set_chunk_index(0);
    req.set_checksum(compute_crc32c("hello", 5));
    req.set_is_last_chunk(true);
    // No block_token set

    protocol::WriteBlockResponse resp;
    stub_->WriteBlock(&cntl, &req, &resp, nullptr);

    ASSERT_FALSE(cntl.Failed());
    EXPECT_EQ(resp.status().code(), static_cast<uint32_t>(ErrorCode::kInvalidBlockToken));
}

TEST_F(BlockTokenAuthTest, WriteBlockWithReadTokenFails) {
    auto token = issue_token(kBlockTokenPermissionRead);

    brpc::Controller cntl;
    protocol::WriteBlockRequest req;
    req.set_block_id(kBlockId);
    req.set_inode_id(kInodeId);
    req.set_block_index(kBlockIndex);
    req.set_generation_stamp(kGenerationStamp);
    req.set_data("hello");
    req.set_chunk_index(0);
    req.set_checksum(compute_crc32c("hello", 5));
    req.set_is_last_chunk(true);
    *req.mutable_block_token() = token;

    protocol::WriteBlockResponse resp;
    stub_->WriteBlock(&cntl, &req, &resp, nullptr);

    ASSERT_FALSE(cntl.Failed());
    EXPECT_EQ(resp.status().code(), static_cast<uint32_t>(ErrorCode::kInvalidBlockToken));
}

// ========== TransferBlock Authorization Tests ==========

TEST_F(BlockTokenAuthTest, TransferBlockWithValidTokenSucceeds) {
    auto token = issue_token(kBlockTokenPermissionTransfer);

    brpc::Controller cntl;
    protocol::TransferBlockRequest req;
    req.set_block_id(kBlockId);
    req.set_generation_stamp(kGenerationStamp);
    req.set_inode_id(kInodeId);
    req.set_block_index(kBlockIndex);
    req.set_data("transfer data");
    *req.mutable_block_token() = token;

    protocol::TransferBlockResponse resp;
    stub_->TransferBlock(&cntl, &req, &resp, nullptr);

    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    EXPECT_EQ(resp.status().code(), 0);
}

TEST_F(BlockTokenAuthTest, TransferBlockRejectsTokenBoundToDifferentInodeOrIndex) {
    auto token = issue_token(kBlockTokenPermissionTransfer);

    protocol::TransferBlockRequest req;
    req.set_block_id(kBlockId);
    req.set_generation_stamp(kGenerationStamp);
    req.set_inode_id(kInodeId + 1);
    req.set_block_index(kBlockIndex);
    req.set_data("transfer data");
    *req.mutable_block_token() = token;

    brpc::Controller cntl;
    protocol::TransferBlockResponse resp;
    stub_->TransferBlock(&cntl, &req, &resp, nullptr);
    EXPECT_EQ(resp.status().code(), static_cast<uint32_t>(ErrorCode::kInvalidBlockToken));

    req.set_inode_id(kInodeId);
    req.set_block_index(kBlockIndex + 1);
    resp.Clear();
    cntl.Reset();
    stub_->TransferBlock(&cntl, &req, &resp, nullptr);
    EXPECT_EQ(resp.status().code(), static_cast<uint32_t>(ErrorCode::kInvalidBlockToken));
}

TEST_F(BlockTokenAuthTest, TransferBlockWithoutTokenFails) {
    brpc::Controller cntl;
    protocol::TransferBlockRequest req;
    req.set_block_id(kBlockId);
    req.set_generation_stamp(kGenerationStamp);
    req.set_inode_id(kInodeId);
    req.set_block_index(kBlockIndex);
    req.set_data("transfer data");
    // No block_token

    protocol::TransferBlockResponse resp;
    stub_->TransferBlock(&cntl, &req, &resp, nullptr);

    ASSERT_FALSE(cntl.Failed());
    EXPECT_EQ(resp.status().code(), static_cast<uint32_t>(ErrorCode::kInvalidBlockToken));
}

TEST_F(BlockTokenAuthTest, TransferBlockWithWriteTokenFails) {
    auto token = issue_token(kBlockTokenPermissionWrite);

    brpc::Controller cntl;
    protocol::TransferBlockRequest req;
    req.set_block_id(kBlockId);
    req.set_generation_stamp(kGenerationStamp);
    req.set_inode_id(kInodeId);
    req.set_block_index(kBlockIndex);
    req.set_data("transfer data");
    *req.mutable_block_token() = token;

    protocol::TransferBlockResponse resp;
    stub_->TransferBlock(&cntl, &req, &resp, nullptr);

    ASSERT_FALSE(cntl.Failed());
    EXPECT_EQ(resp.status().code(), static_cast<uint32_t>(ErrorCode::kInvalidBlockToken));
}

// ========== TruncateBlock Authorization Tests ==========

TEST_F(BlockTokenAuthTest, TruncateBlockWithValidTokenSucceeds) {
    // First create and finalize a block
    ASSERT_TRUE(store_->create_block(kBlockId, kInodeId, kBlockIndex, kGenerationStamp).hasValue());
    std::string data = "some data to truncate";
    ASSERT_TRUE(
        store_->append_chunk(kBlockId, kGenerationStamp, data.data(), data.size(), 0).hasValue());
    ASSERT_TRUE(store_->finalize_block(kBlockId, kGenerationStamp).hasValue());

    auto token = issue_token(kBlockTokenPermissionTruncate);

    brpc::Controller cntl;
    protocol::TruncateBlockRequest req;
    req.set_block_id(kBlockId);
    req.set_generation_stamp(kGenerationStamp);
    req.set_length(5);
    *req.mutable_block_token() = token;

    protocol::TruncateBlockResponse resp;
    stub_->TruncateBlock(&cntl, &req, &resp, nullptr);

    ASSERT_FALSE(cntl.Failed()) << cntl.ErrorText();
    EXPECT_EQ(resp.status().code(), 0);
}

TEST_F(BlockTokenAuthTest, TruncateBlockWithoutTokenFails) {
    // Create a block so truncate would otherwise succeed
    ASSERT_TRUE(store_->create_block(kBlockId, kInodeId, kBlockIndex, kGenerationStamp).hasValue());
    std::string data = "some data";
    ASSERT_TRUE(
        store_->append_chunk(kBlockId, kGenerationStamp, data.data(), data.size(), 0).hasValue());
    ASSERT_TRUE(store_->finalize_block(kBlockId, kGenerationStamp).hasValue());

    brpc::Controller cntl;
    protocol::TruncateBlockRequest req;
    req.set_block_id(kBlockId);
    req.set_generation_stamp(kGenerationStamp);
    req.set_length(5);
    // No block_token

    protocol::TruncateBlockResponse resp;
    stub_->TruncateBlock(&cntl, &req, &resp, nullptr);

    ASSERT_FALSE(cntl.Failed());
    EXPECT_EQ(resp.status().code(), static_cast<uint32_t>(ErrorCode::kInvalidBlockToken));
}

TEST_F(BlockTokenAuthTest, TruncateBlockWithReadTokenFails) {
    // Create a block so truncate would otherwise succeed
    ASSERT_TRUE(store_->create_block(kBlockId, kInodeId, kBlockIndex, kGenerationStamp).hasValue());
    std::string data = "some data";
    ASSERT_TRUE(
        store_->append_chunk(kBlockId, kGenerationStamp, data.data(), data.size(), 0).hasValue());
    ASSERT_TRUE(store_->finalize_block(kBlockId, kGenerationStamp).hasValue());

    auto token = issue_token(kBlockTokenPermissionRead);

    brpc::Controller cntl;
    protocol::TruncateBlockRequest req;
    req.set_block_id(kBlockId);
    req.set_generation_stamp(kGenerationStamp);
    req.set_length(5);
    *req.mutable_block_token() = token;

    protocol::TruncateBlockResponse resp;
    stub_->TruncateBlock(&cntl, &req, &resp, nullptr);

    ASSERT_FALSE(cntl.Failed());
    EXPECT_EQ(resp.status().code(), static_cast<uint32_t>(ErrorCode::kInvalidBlockToken));
}

// ========== Block Token Primitive Tests ==========

TEST(BlockTokenPrimitiveTest, RefreshesExpiredAndNearlyExpiredTokens) {
    protocol::BlockTokenProto token;
    token.set_expires_at_ms(2000);
    EXPECT_TRUE(block_token_needs_refresh(token, 2000, 100));
    EXPECT_TRUE(block_token_needs_refresh(token, 1950, 100));
    EXPECT_FALSE(block_token_needs_refresh(token, 1800, 100));
}

TEST(BlockTokenPrimitiveTest, RequiresExplicitConfiguredSecret) {
    EXPECT_EQ(configured_block_token_secret("explicit-secret"), "explicit-secret");

    const char* original = std::getenv("MINIDFS_BLOCK_TOKEN_SECRET");
    const std::optional<std::string> saved =
        original == nullptr ? std::nullopt : std::optional<std::string>(original);
    ASSERT_EQ(setenv("MINIDFS_BLOCK_TOKEN_SECRET", "environment-secret", 1), 0);
    EXPECT_EQ(configured_block_token_secret(), "environment-secret");
    if (saved.has_value()) {
        ASSERT_EQ(setenv("MINIDFS_BLOCK_TOKEN_SECRET", saved->c_str(), 1), 0);
    } else {
        ASSERT_EQ(unsetenv("MINIDFS_BLOCK_TOKEN_SECRET"), 0);
        EXPECT_TRUE(configured_block_token_secret().empty());
    }
    EXPECT_EQ(configured_block_token_secret("explicit-secret"), "explicit-secret");
}

TEST(BlockTokenPrimitiveTest, ConstantTimeEqualsRejectsDifferentLengths) {
    EXPECT_FALSE(constant_time_equals("abc", "abcd"));
    EXPECT_FALSE(constant_time_equals("", "a"));
}

TEST(BlockTokenPrimitiveTest, ConstantTimeEqualsAcceptsIdentical) {
    EXPECT_TRUE(constant_time_equals("hello", "hello"));
    EXPECT_TRUE(constant_time_equals("", ""));
}

TEST(BlockTokenPrimitiveTest, ConstantTimeEqualsRejectsDifferentContent) {
    EXPECT_FALSE(constant_time_equals("hello", "hella"));
    EXPECT_FALSE(constant_time_equals("abc", "abd"));
}

TEST(BlockTokenPrimitiveTest, IssueAndVerifyTokenRoundTrip) {
    const std::string secret = "round-trip-test-secret";
    auto token = issue_block_token(100, 200, 300, 5, kBlockTokenPermissionRead, 300000, secret);

    EXPECT_TRUE(verify_block_token(token, secret, BlockTokenPermission::kRead, 100, 200, 300, 5));
    EXPECT_FALSE(verify_block_token(token, secret, BlockTokenPermission::kWrite, 100, 200, 300, 5));
    EXPECT_FALSE(
        verify_block_token(token, "wrong-secret", BlockTokenPermission::kRead, 100, 200, 300, 5));
    EXPECT_FALSE(verify_block_token(token, secret, BlockTokenPermission::kRead, 999, 200, 300, 5));
}

TEST(BlockTokenPrimitiveTest, FileIdentityIsCoveredBySignature) {
    const std::string secret = "identity-test-secret";
    protocol::FileIdentityProto identity;
    identity.set_inode_id(300);
    identity.set_content_generation(7);
    identity.set_length(4096);
    identity.set_checksum(0x12345678u);
    identity.set_checksum_valid(true);

    auto token = issue_block_token(
        100, 200, 300, 5, kBlockTokenPermissionRead, 300000, secret, 1000, &identity);
    ASSERT_TRUE(token.has_file_identity());
    EXPECT_TRUE(verify_block_token(
        token, secret, BlockTokenPermission::kRead, 100, 200, 300, 5, 2000));

    token.mutable_file_identity()->set_content_generation(8);
    EXPECT_FALSE(verify_block_token(
        token, secret, BlockTokenPermission::kRead, 100, 200, 300, 5, 2000));
}

TEST(BlockTokenPrimitiveTest, ExpiredTokenFailsVerification) {
    const std::string secret = "expiry-test-secret";
    // Issue token with 0 TTL — already expired
    auto token =
        issue_block_token(100, 200, 300, 5, kBlockTokenPermissionRead, 0, secret, /*now=*/1000);
    // Token expires at 1000ms, current time is after that
    EXPECT_FALSE(
        verify_block_token(token, secret, BlockTokenPermission::kRead, 100, 200, 300, 5, 2000));
}

TEST(BlockTokenPrimitiveTest, TamperedSignatureFailsVerification) {
    const std::string secret = "tamper-test-secret";
    auto token = issue_block_token(100, 200, 300, 5, kBlockTokenPermissionRead, 300000, secret);

    // Tamper with the signature
    std::string tampered_sig = token.signature();
    if (!tampered_sig.empty()) {
        tampered_sig[0] ^= 0xFF;
    }
    token.set_signature(tampered_sig);

    EXPECT_FALSE(verify_block_token(token, secret, BlockTokenPermission::kRead, 100, 200, 300, 5));
}

} // namespace
} // namespace pl::minidfs
