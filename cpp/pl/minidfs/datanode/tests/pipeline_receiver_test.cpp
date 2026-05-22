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

#include <filesystem>
#include <gtest/gtest.h>
#include <string>

#include "cpp/pl/minidfs/common/checksum.h"
#include "cpp/pl/minidfs/datanode/pipeline_receiver.h"

namespace pl::minidfs {
namespace {

namespace fs = std::filesystem;

/// TestPipelineReceiver overrides the network forwarding methods for unit testing.
class TestPipelineReceiver : public PipelineReceiver {
public:
    using PipelineReceiver::PipelineReceiver;

    int forward_count = 0;
    int forward_finalize_count = 0;
    AckStatus forward_ack = AckStatus::kSuccess;
    bool forward_finalize_error = false;

protected:
    Result<AckStatus> forward_to_downstream(const PacketHeader& /*header*/,
                                            const void* /*data*/,
                                            uint32_t /*data_length*/) override {
        forward_count++;
        return forward_ack;
    }

    Result<Void> forward_finalize(uint64_t /*block_id*/, uint64_t /*generation_stamp*/) override {
        forward_finalize_count++;
        if (forward_finalize_error) {
            return pl::makeError(pl::Status(5000, "downstream finalize failed"));
        }
        RETURN_VOID;
    }
};

class PipelineReceiverTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ =
            fs::temp_directory_path() / ("minidfs_pipeline_test_" + std::to_string(::getpid()) +
                                         "_" + std::to_string(counter_++));
        fs::create_directories(test_dir_);

        LocalBlockStore::Config config;
        config.storage_root = test_dir_.string();
        config.reserved_bytes = 0;
        store_ = std::make_unique<LocalBlockStore>(std::move(config));
        store_->init();
    }

    void TearDown() override {
        store_.reset();
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    PacketHeader make_packet(
        uint64_t block_id, uint64_t gs, uint32_t chunk_idx, const void* data, uint32_t len) {
        PacketHeader hdr{};
        hdr.block_id = block_id;
        hdr.generation_stamp = gs;
        hdr.chunk_index = chunk_idx;
        hdr.data_length = len;
        hdr.checksum = compute_crc32c(data, len);
        return hdr;
    }

    fs::path test_dir_;
    std::unique_ptr<LocalBlockStore> store_;
    static inline int counter_ = 0;
};

// ============================================================================
// setup tests
// ============================================================================

TEST_F(PipelineReceiverTest, SetupCreatesBlockInTmp) {
    TestPipelineReceiver receiver(store_.get());
    auto result = receiver.setup(100, 1, 0, 5000, {});
    ASSERT_TRUE(result.hasValue());

    // Block should exist in tmp/
    EXPECT_TRUE(fs::exists(test_dir_ / "tmp" / "blk_100_5000.blk"));
}

TEST_F(PipelineReceiverTest, SetupWithDownstreamTargets) {
    TestPipelineReceiver receiver(store_.get());
    std::vector<PipelineTarget> targets = {
        {.host = "dn2", .data_port = 9001},
        {.host = "dn3", .data_port = 9002},
    };
    auto result = receiver.setup(101, 1, 0, 5001, targets);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(receiver.downstream_targets().size(), 2u);
}

// ============================================================================
// receive_packet tests
// ============================================================================

TEST_F(PipelineReceiverTest, ReceivePacketSuccess) {
    TestPipelineReceiver receiver(store_.get());
    receiver.setup(200, 1, 0, 6000, {});

    std::string data = "hello pipeline";
    auto hdr = make_packet(200, 6000, 0, data.data(), data.size());
    auto result = receiver.receive_packet(hdr, data.data(), data.size());
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value(), AckStatus::kSuccess);
}

TEST_F(PipelineReceiverTest, ReceivePacketChecksumMismatch) {
    TestPipelineReceiver receiver(store_.get());
    receiver.setup(201, 1, 0, 6001, {});

    std::string data = "some data";
    PacketHeader hdr{};
    hdr.block_id = 201;
    hdr.generation_stamp = 6001;
    hdr.chunk_index = 0;
    hdr.data_length = data.size();
    hdr.checksum = 0xDEADBEEF; // wrong checksum

    auto result = receiver.receive_packet(hdr, data.data(), data.size());
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value(), AckStatus::kChecksumError);
}

TEST_F(PipelineReceiverTest, ReceivePacketWrongBlock) {
    TestPipelineReceiver receiver(store_.get());
    receiver.setup(202, 1, 0, 6002, {});

    std::string data = "x";
    auto hdr = make_packet(999, 6002, 0, data.data(), data.size()); // wrong block_id
    auto result = receiver.receive_packet(hdr, data.data(), data.size());
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value(), AckStatus::kIOError);
}

TEST_F(PipelineReceiverTest, ReceivePacketForwardsToDownstream) {
    TestPipelineReceiver receiver(store_.get());
    std::vector<PipelineTarget> targets = {{.host = "dn2", .data_port = 9001}};
    receiver.setup(203, 1, 0, 6003, targets);

    std::string data = "forwarded data";
    auto hdr = make_packet(203, 6003, 0, data.data(), data.size());
    auto result = receiver.receive_packet(hdr, data.data(), data.size());
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value(), AckStatus::kSuccess);
    EXPECT_EQ(receiver.forward_count, 1);
}

TEST_F(PipelineReceiverTest, ReceivePacketDownstreamFailure) {
    TestPipelineReceiver receiver(store_.get());
    receiver.forward_ack = AckStatus::kIOError; // simulate downstream failure
    std::vector<PipelineTarget> targets = {{.host = "dn2", .data_port = 9001}};
    receiver.setup(204, 1, 0, 6004, targets);

    std::string data = "will fail downstream";
    auto hdr = make_packet(204, 6004, 0, data.data(), data.size());
    auto result = receiver.receive_packet(hdr, data.data(), data.size());
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value(), AckStatus::kIOError);
}

// ============================================================================
// finalize tests
// ============================================================================

TEST_F(PipelineReceiverTest, FinalizeSuccess) {
    TestPipelineReceiver receiver(store_.get());
    receiver.setup(300, 1, 0, 7000, {});

    std::string data = "finalize me";
    auto hdr = make_packet(300, 7000, 0, data.data(), data.size());
    receiver.receive_packet(hdr, data.data(), data.size());

    auto result = receiver.finalize(300, 7000);
    ASSERT_TRUE(result.hasValue());

    // Block should be in current/ now
    EXPECT_TRUE(store_->has_block(300, 7000));
}

TEST_F(PipelineReceiverTest, FinalizeWithDownstream) {
    TestPipelineReceiver receiver(store_.get());
    std::vector<PipelineTarget> targets = {{.host = "dn2", .data_port = 9001}};
    receiver.setup(301, 1, 0, 7001, targets);

    std::string data = "data";
    auto hdr = make_packet(301, 7001, 0, data.data(), data.size());
    receiver.receive_packet(hdr, data.data(), data.size());

    auto result = receiver.finalize(301, 7001);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(receiver.forward_finalize_count, 1);
}

TEST_F(PipelineReceiverTest, FinalizeWrongBlockFails) {
    TestPipelineReceiver receiver(store_.get());
    receiver.setup(302, 1, 0, 7002, {});

    auto result = receiver.finalize(999, 7002); // wrong block id
    EXPECT_TRUE(result.hasError());
}

TEST_F(PipelineReceiverTest, FinalizeDownstreamError) {
    TestPipelineReceiver receiver(store_.get());
    receiver.forward_finalize_error = true;
    std::vector<PipelineTarget> targets = {{.host = "dn2", .data_port = 9001}};
    receiver.setup(303, 1, 0, 7003, targets);

    std::string data = "data";
    auto hdr = make_packet(303, 7003, 0, data.data(), data.size());
    receiver.receive_packet(hdr, data.data(), data.size());

    auto result = receiver.finalize(303, 7003);
    EXPECT_TRUE(result.hasError());
}

// ============================================================================
// abort tests
// ============================================================================

TEST_F(PipelineReceiverTest, AbortRemovesTmpBlock) {
    TestPipelineReceiver receiver(store_.get());
    receiver.setup(400, 1, 0, 8000, {});

    EXPECT_TRUE(fs::exists(test_dir_ / "tmp" / "blk_400_8000.blk"));

    receiver.abort(400, 8000);
    EXPECT_FALSE(fs::exists(test_dir_ / "tmp" / "blk_400_8000.blk"));
}

TEST_F(PipelineReceiverTest, AbortNonexistentNocrash) {
    TestPipelineReceiver receiver(store_.get());
    receiver.setup(401, 1, 0, 8001, {});
    // Abort a different block — should not crash
    receiver.abort(999, 1);
}

// ============================================================================
// Full pipeline lifecycle
// ============================================================================

TEST_F(PipelineReceiverTest, FullLifecycle) {
    TestPipelineReceiver receiver(store_.get());
    std::vector<PipelineTarget> targets = {{.host = "dn2", .data_port = 9001}};
    receiver.setup(500, 42, 0, 9000, targets);

    std::string chunk1 = "chunk_one_data";
    std::string chunk2 = "chunk_two_data";

    auto hdr1 = make_packet(500, 9000, 0, chunk1.data(), chunk1.size());
    auto r1 = receiver.receive_packet(hdr1, chunk1.data(), chunk1.size());
    ASSERT_TRUE(r1.hasValue());
    EXPECT_EQ(r1.value(), AckStatus::kSuccess);

    auto hdr2 = make_packet(500, 9000, 1, chunk2.data(), chunk2.size());
    auto r2 = receiver.receive_packet(hdr2, chunk2.data(), chunk2.size());
    ASSERT_TRUE(r2.hasValue());
    EXPECT_EQ(r2.value(), AckStatus::kSuccess);

    auto fin = receiver.finalize(500, 9000);
    ASSERT_TRUE(fin.hasValue());

    EXPECT_TRUE(store_->has_block(500, 9000));
    EXPECT_EQ(receiver.forward_count, 2);
    EXPECT_EQ(receiver.forward_finalize_count, 1);

    // Verify data integrity
    auto data = store_->read_block_data(500, 9000);
    ASSERT_TRUE(data.hasValue());
    EXPECT_EQ(data.value(), "chunk_one_datachunk_two_data");
}

} // namespace
} // namespace pl::minidfs
